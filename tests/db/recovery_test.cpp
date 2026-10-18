// NexusDB — R2 crash-recovery and write-ahead-ordering test.
//
// Exercises real persistence, not mock log records:
//   - BufferPool enforces write-ahead ordering (a failed log flush blocks the
//     dependent data-page write) across FlushAll, FlushPage and eviction.
//   - Recovery after a durable log record but before the data-page write.
//   - Recovery after some, but not all, data pages were persisted.
//   - Database open/recovery stays correct after an abrupt child-process
//     termination, including a crash injected exactly between the durable log
//     write and the data-page write.
//
// Same plain PASS/FAIL style as the other DB tests: exit 0 on success, 1 on any
// failure. No timing assumptions.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/database.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/query_executor.h"
#include "db/wal.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#define NEXUS_HAVE_FORK 1
#else
#define NEXUS_HAVE_FORK 0
#endif

namespace {

using nexusdb::BufferPool;
using nexusdb::Database;
using nexusdb::DbError;
using nexusdb::Page;
using nexusdb::PageLogSink;
using nexusdb::PageStorage;
using nexusdb::QueryExecutor;
using nexusdb::Status;
using nexusdb::Wal;

int g_failures = 0;

#define CHECK(desc, expr)                     \
  do {                                        \
    if (expr) {                               \
      printf("PASS: %s\n", desc);             \
    } else {                                  \
      printf("FAIL: %s\n", desc);             \
      ++g_failures;                           \
    }                                         \
  } while (0)

constexpr std::size_t kRec = Wal::kHeaderSize + Wal::kPageImagePayload;

std::string TestPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void RemoveFile(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void FillPage(std::uint8_t* p, std::uint8_t seed, Page::PageId id) {
  for (std::size_t i = 0; i < Page::size(); ++i) {
    p[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
  }
  p[0] = static_cast<std::uint8_t>(id & 0xFFu);
  p[1] = static_cast<std::uint8_t>((id >> 8) & 0xFFu);
}

bool PageMatches(const std::uint8_t* p, std::uint8_t seed, Page::PageId id) {
  if (p[0] != static_cast<std::uint8_t>(id & 0xFFu) ||
      p[1] != static_cast<std::uint8_t>((id >> 8) & 0xFFu)) {
    return false;
  }
  for (std::size_t i = 2; i < Page::size(); ++i) {
    if (p[i] != static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i))) {
      return false;
    }
  }
  return true;
}

bool ReadPageBytes(PageStorage* storage, Page::PageId id,
                   std::vector<std::uint8_t>* out) {
  Page page;
  if (!storage->ReadPage(id, &page).ok()) {
    return false;
  }
  out->assign(page.data(), page.data() + Page::size());
  return true;
}

std::uint64_t FileSize(const std::string& path) {
  std::error_code ec;
  const auto n = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

std::unique_ptr<PageStorage> OpenStorage(const std::string& path) {
  std::unique_ptr<PageStorage> storage;
  if (!PageStorage::Open(path, &storage).ok()) {
    return nullptr;
  }
  return storage;
}

// A sink that always fails, modelling a failed log flush.
class FailingSink : public PageLogSink {
 public:
  Status BeforePageWrite(const Page&) override {
    return Status::Error(DbError::kIoError, "injected log flush failure");
  }
};

void FillPattern(std::uint8_t* p, std::size_t n, std::uint8_t seed) {
  for (std::size_t i = 0; i < n; ++i) {
    p[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
  }
}

bool MatchesPattern(const std::uint8_t* p, std::size_t n, std::uint8_t seed) {
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] != static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i))) {
      return false;
    }
  }
  return true;
}

struct ScanState {
  std::size_t count = 0;
  std::size_t expect_len = 0;
  std::uint8_t seed = 0;
  bool all_match = true;
};

bool ScanCb(const QueryExecutor::RecordId&, const std::uint8_t* data,
            std::size_t len, void* ctx) {
  ScanState* st = static_cast<ScanState*>(ctx);
  ++st->count;
  if (len != st->expect_len || !MatchesPattern(data, len, st->seed)) {
    st->all_match = false;
  }
  return true;
}

#if NEXUS_HAVE_FORK
// Logs the page image durably to `wal`, then crashes the process immediately --
// before BufferPool can write the data page. This lands the process exactly in
// the "durable log record, no data-page write" window.
class CrashAfterLogSink : public PageLogSink {
 public:
  explicit CrashAfterLogSink(Wal* wal) : wal_(wal) {}
  Status BeforePageWrite(const Page& page) override {
    Status s =
        wal_->AppendPageImage(page.id(), page.data(), Page::size(), nullptr);
    if (!s.ok()) {
      return s;
    }
    s = wal_->Flush();
    if (!s.ok()) {
      return s;
    }
    _exit(0);  // abrupt termination: no destructors, no data-page write
  }

 private:
  Wal* wal_;
};

void InsertRecords(QueryExecutor& ex, const std::string& table, int count,
                   std::size_t len, std::uint8_t seed) {
  std::vector<std::uint8_t> buf(len);
  for (int i = 0; i < count; ++i) {
    FillPattern(buf.data(), len, static_cast<std::uint8_t>(seed + i));
    QueryExecutor::RecordId id;
    ex.Insert(table, buf.data(), len, &id);
  }
}

// Collects scanned records so the parent can verify recovered content.
struct RecCollector {
  struct Item {
    QueryExecutor::RecordId id;
    std::uint8_t first;
    std::size_t len;
  };
  Item items[8];
  std::size_t count = 0;
};

bool CollectCb(const QueryExecutor::RecordId& id, const std::uint8_t* data,
               std::size_t len, void* ctx) {
  RecCollector* c = static_cast<RecCollector*>(ctx);
  if (c->count < 8) {
    c->items[c->count].id = id;
    c->items[c->count].first = len > 0 ? data[0] : 0;
    c->items[c->count].len = len;
  }
  ++c->count;
  return true;
}
#endif  // NEXUS_HAVE_FORK

}  // namespace

int main() {
  printf("=== NexusDB R2: crash-recovery test ===\n\n");

  // ---- 1. A failed log flush blocks the dependent data-page write ----
  {
    const std::string db_path = TestPath("nexusdb_recover_block.db");
    RemoveFile(db_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);
    {
      std::vector<std::uint8_t> old(Page::size());
      FillPage(old.data(), 0x01, 1);
      std::memcpy(p1.data(), old.data(), Page::size());
      storage->WritePage(p1);
    }

    std::unique_ptr<BufferPool> pool;
    BufferPool::Create(storage.get(), 4, &pool);
    {
      BufferPool::Handle h;
      pool->Fetch(1, &h);
      FillPage(h.page().data(), 0x02, 1);
      h.MarkDirty();
    }

    FailingSink sink;
    pool->SetLogSink(&sink);
    Status s = pool->FlushAll();
    CHECK("FlushAll with a failing log flush returns the log error",
          !s.ok() && s.code == DbError::kIoError);

    {
      BufferPool::Handle h;
      CHECK("the page is still resident after the failed flush",
            pool->Fetch(1, &h).ok());
      CHECK("the page is still dirty after the failed flush", h.IsDirty());
    }
    {
      std::unique_ptr<PageStorage> reader = OpenStorage(db_path);
      std::vector<std::uint8_t> got;
      CHECK("the data page was NOT written ahead of the log",
            reader != nullptr && ReadPageBytes(reader.get(), 1, &got) &&
                PageMatches(got.data(), 0x01, 1));
    }

    // With the log healthy the write proceeds.
    pool->SetLogSink(nullptr);
    CHECK("FlushAll succeeds once the log sink is cleared",
          pool->FlushAll().ok());
    {
      std::unique_ptr<PageStorage> reader = OpenStorage(db_path);
      std::vector<std::uint8_t> got;
      CHECK("the data page is written once the log is durable",
            reader != nullptr && ReadPageBytes(reader.get(), 1, &got) &&
                PageMatches(got.data(), 0x02, 1));
    }
    pool.reset();
    storage->Close();
    RemoveFile(db_path);
  }

  // ---- 2. FlushPage logs the page image before writing it ----
  {
    const std::string db_path = TestPath("nexusdb_recover_flushpage.db");
    const std::string wal_path = TestPath("nexusdb_recover_flushpage.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);
    std::unique_ptr<Wal> wal;
    Wal::Open(wal_path, &wal);

    std::unique_ptr<BufferPool> pool;
    BufferPool::Create(storage.get(), 4, &pool);
    pool->SetLogSink(wal.get());
    {
      BufferPool::Handle h;
      pool->Fetch(1, &h);
      FillPage(h.page().data(), 0x07, 1);
      h.MarkDirty();
    }
    CHECK("FlushPage succeeds", pool->FlushPage(1).ok());
    CHECK("the flush wrote exactly one log record", FileSize(wal_path) == kRec);
    {
      std::unique_ptr<PageStorage> reader = OpenStorage(db_path);
      std::vector<std::uint8_t> got;
      CHECK("the data page carries the logged image",
            reader != nullptr && ReadPageBytes(reader.get(), 1, &got) &&
                PageMatches(got.data(), 0x07, 1));
    }
    pool.reset();
    wal->Close();
    storage->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 3. Eviction respects write-ahead ordering ----
  {
    const std::string db_path = TestPath("nexusdb_recover_evict.db");
    const std::string wal_path = TestPath("nexusdb_recover_evict.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1, p2;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);
    storage->AllocatePage(&p2);
    std::unique_ptr<Wal> wal;
    Wal::Open(wal_path, &wal);

    std::unique_ptr<BufferPool> pool;
    BufferPool::Create(storage.get(), 1, &pool);  // one frame: forces eviction
    pool->SetLogSink(wal.get());
    {
      BufferPool::Handle h;
      pool->Fetch(1, &h);
      FillPage(h.page().data(), 0x08, 1);
      h.MarkDirty();
    }
    CHECK("no log record before eviction", FileSize(wal_path) == 0);
    {
      BufferPool::Handle h;
      CHECK("fetching another page evicts the dirty one",
            pool->Fetch(2, &h).ok());
    }
    CHECK("eviction logged the evicted page before writing it",
          FileSize(wal_path) == kRec);
    {
      std::unique_ptr<PageStorage> reader = OpenStorage(db_path);
      std::vector<std::uint8_t> got;
      CHECK("the evicted page is durable with the logged image",
            reader != nullptr && ReadPageBytes(reader.get(), 1, &got) &&
                PageMatches(got.data(), 0x08, 1));
    }
    pool.reset();
    wal->Close();
    storage->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 4. Recovery after some, but not all, data pages were persisted ----
  {
    const std::string db_path = TestPath("nexusdb_recover_partial.db");
    const std::string wal_path = TestPath("nexusdb_recover_partial.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1, p2, p3;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);
    storage->AllocatePage(&p2);
    storage->AllocatePage(&p3);

    std::unique_ptr<Wal> wal;
    Wal::Open(wal_path, &wal);
    std::vector<std::uint8_t> buf(Page::size());
    FillPage(buf.data(), 0xa1, 1);
    wal->AppendPageImage(1, buf.data(), buf.size());
    FillPage(buf.data(), 0xa2, 2);
    wal->AppendPageImage(2, buf.data(), buf.size());
    FillPage(buf.data(), 0xa3, 3);
    wal->AppendPageImage(3, buf.data(), buf.size());
    CHECK("three images logged", wal->Flush().ok());

    // Only page 1 reached storage before the crash.
    Page persisted(1);
    FillPage(persisted.data(), 0xa1, 1);
    storage->WritePage(persisted);

    Wal::RecoveryStats stats;
    CHECK("recovery replays the remaining pages",
          wal->Recover(storage.get(), &stats).ok());
    CHECK("all three images were applied", stats.records_applied == 3);
    std::vector<std::uint8_t> got;
    CHECK("the already-persisted page is unchanged",
          ReadPageBytes(storage.get(), 1, &got) &&
              PageMatches(got.data(), 0xa1, 1));
    CHECK("the not-yet-persisted page 2 is recovered",
          ReadPageBytes(storage.get(), 2, &got) &&
              PageMatches(got.data(), 0xa2, 2));
    CHECK("the not-yet-persisted page 3 is recovered",
          ReadPageBytes(storage.get(), 3, &got) &&
              PageMatches(got.data(), 0xa3, 3));
    CHECK("page 0 is untouched",
          ReadPageBytes(storage.get(), 0, &got) &&
              !PageMatches(got.data(), 0xa1, 1));

    wal->Close();
    storage->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 5. Database: normal create / close / reopen persistence ----
  {
    const std::string db_path = TestPath("nexusdb_db_persist.db");
    const std::string wal_path = TestPath("nexusdb_db_persist.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    QueryExecutor::RecordId saved;
    Page::PageId catalog_root = Page::kInvalidId;
    {
      std::unique_ptr<Database> db;
      CHECK("open a fresh database",
            Database::Open(db_path, wal_path, 16, &db).ok());
      CHECK("database reports a catalog root",
            db != nullptr &&
                (catalog_root = db->catalog_root()) != Page::kInvalidId);
      CHECK("create table", db->executor().CreateTable("events").ok());
      std::vector<std::uint8_t> rec(24);
      FillPattern(rec.data(), rec.size(), 0x40);
      CHECK("insert record",
            db->executor().Insert("events", rec.data(), rec.size(), &saved)
                .ok());
      CHECK("clean close", db->Close().ok());
    }
    {
      std::unique_ptr<Database> db;
      CHECK("reopen the database",
            Database::Open(db_path, wal_path, 16, &db).ok());
      CHECK("catalog root is stable", db != nullptr &&
                                          db->catalog_root() == catalog_root);
      CHECK("table survives reopen", db->executor().TableExists("events"));
      const std::uint8_t* data = nullptr;
      std::size_t len = 0;
      CHECK("record survives reopen with its bytes",
            db->executor().Get("events", saved, &data, &len).ok() &&
                len == 24 && MatchesPattern(data, len, 0x40));
      db->Close();
    }
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

#if NEXUS_HAVE_FORK
  // ---- 6. Recovery after a crash between the durable log record and the
  //         data-page write (real fork, no graceful shutdown) ----
  {
    const std::string db_path = TestPath("nexusdb_db_window.db");
    const std::string wal_path = TestPath("nexusdb_db_window.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);

    const pid_t pid = fork();
    CHECK("fork succeeded", pid >= 0);
    if (pid == 0) {
      // Child.
      std::unique_ptr<Database> db;
      if (!Database::Open(db_path, wal_path, 64, &db).ok()) {
        _exit(11);
      }
      if (!db->executor().CreateTable("crash").ok()) {
        _exit(12);
      }
      // First record, then flush it durably so the directory page is clean.
      InsertRecords(db->executor(), "crash", 1, 40, 0x60);
      if (!db->pool()->FlushAll().ok()) {
        _exit(13);
      }
      // Second record fits in the SAME data page, so only the data page is
      // dirty now. Crash in the window: the sink logs the data page durably,
      // then _exit before BufferPool writes it.
      InsertRecords(db->executor(), "crash", 1, 40, 0x61);
      CrashAfterLogSink sink(db->wal());
      db->pool()->SetLogSink(&sink);
      db->pool()->FlushAll();  // should never return
      _exit(14);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("child reached the crash window and terminated abruptly",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    std::unique_ptr<Database> db;
    CHECK("reopen recovers from the log",
          Database::Open(db_path, wal_path, 64, &db).ok());
    CHECK("table survived recovery", db->executor().TableExists("crash"));
    RecCollector recs;
    CHECK("scan succeeds after recovery",
          db->executor().Scan("crash", CollectCb, &recs).ok());
    CHECK("both records are recovered", recs.count == 2);
    bool content_ok = recs.count == 2 && recs.items[0].len == 40 &&
                      recs.items[1].len == 40 &&
                      recs.items[0].first == 0x60 && recs.items[1].first == 0x61;
    for (std::size_t i = 0; i < 2 && content_ok; ++i) {
      const std::uint8_t* data = nullptr;
      std::size_t len = 0;
      content_ok = db->executor()
                       .Get("crash", recs.items[i].id, &data, &len)
                       .ok() &&
                   MatchesPattern(data, len,
                                  static_cast<std::uint8_t>(0x60 + i));
    }
    CHECK("recovered records carry the logged bytes", content_ok);
    db->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 7. Recovery after abrupt termination following an explicit flush ----
  {
    const std::string db_path = TestPath("nexusdb_db_flush.db");
    const std::string wal_path = TestPath("nexusdb_db_flush.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);

    const pid_t pid = fork();
    CHECK("fork succeeded", pid >= 0);
    if (pid == 0) {
      // Child.
      std::unique_ptr<Database> db;
      if (!Database::Open(db_path, wal_path, 64, &db).ok()) {
        _exit(21);
      }
      if (!db->executor().CreateTable("keep").ok()) {
        _exit(22);
      }
      InsertRecords(db->executor(), "keep", 3, 32, 0x30);
      if (!db->pool()->FlushAll().ok()) {  // durably logs + writes 3 records
        _exit(23);
      }
      InsertRecords(db->executor(), "keep", 1, 32, 0x90);  // dirty, unflushed
      _exit(0);  // abrupt termination: the 4th record is never made durable
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("child terminated abruptly with success", WIFEXITED(status) &&
                                                       WEXITSTATUS(status) == 0);

    std::unique_ptr<Database> db;
    CHECK("reopen the crashed database",
          Database::Open(db_path, wal_path, 64, &db).ok());
    ScanState st;
    st.expect_len = 32;
    st.seed = 0x30;
    CHECK("scan succeeds after abrupt termination",
          db->executor().Scan("keep", ScanCb, &st).ok());
    CHECK("the three durable records are recovered", st.count == 3);
    db->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 8. Creation is crash-atomic at every initialization boundary ----
  {
    const char* points[] = {"after-temp-page0", "after-catalog", "after-header",
                            "before-publish", "after-publish"};
    for (const char* point : points) {
      const std::string db_path = TestPath("nexusdb_db_create.db");
      const std::string wal_path = TestPath("nexusdb_db_create.wal");
      const std::string tmp_path = db_path + ".creating";
      RemoveFile(db_path);
      RemoveFile(wal_path);
      RemoveFile(tmp_path);

      const pid_t pid = fork();
      CHECK("create-boundary fork succeeded", pid >= 0);
      if (pid == 0) {
        setenv("NEXUSDB_CRASH_AT", point, 1);
        std::unique_ptr<Database> db;
        Database::Open(db_path, wal_path, 16, &db);  // crashes at `point`
        _exit(99);  // injected crash did not fire
      }

      int status = 0;
      waitpid(pid, &status, 0);
      CHECK("child crashed at the injected creation boundary",
            WIFEXITED(status) && WEXITSTATUS(status) == 70);

      // Whatever boundary was hit, a fresh reopen must yield a usable database
      // and must never produce an unrecoverable file.
      std::unique_ptr<Database> db;
      CHECK("reopen after interrupted creation succeeds",
            Database::Open(db_path, wal_path, 16, &db).ok());
      CHECK("interrupted creation leaves a usable database",
            db != nullptr && db->executor().CreateTable("t").ok());
      if (db != nullptr) {
        db->Close();
      }
      RemoveFile(db_path);
      RemoveFile(wal_path);
      RemoveFile(tmp_path);
    }
  }
#else
  printf("SKIP: fork-based crash tests require a POSIX platform\n");
#endif

  // ---- 9. A fresh database discards a stale write-ahead log ----
  {
    const std::string db_path = TestPath("nexusdb_db_stalewal.db");
    const std::string wal_path = TestPath("nexusdb_db_stalewal.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    {
      std::unique_ptr<Wal> wal;
      CHECK("open a stale log", Wal::Open(wal_path, &wal).ok());
      std::vector<std::uint8_t> buf(Page::size());
      FillPage(buf.data(), 0x5a, 5);
      CHECK("plant a stale log record",
            wal->AppendPageImage(5, buf.data(), buf.size()).ok());
      CHECK("stale log flushed", wal->Flush().ok());
      wal->Close();
    }
    CHECK("stale log is present before open", FileSize(wal_path) > 0);

    std::unique_ptr<Database> db;
    CHECK("open fresh with a stale log present",
          Database::Open(db_path, wal_path, 16, &db).ok());
    CHECK("fresh database is usable (stale log discarded)",
          db != nullptr && db->executor().CreateTable("t").ok());
    if (db != nullptr) {
      db->Close();
    }
    db.reset();
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
      Page page;
      CHECK("stale image was not replayed into the new database",
            storage != nullptr && !storage->ReadPage(5, &page).ok());
    }
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 10. A zero-length database file is treated as brand new ----
  {
    const std::string db_path = TestPath("nexusdb_db_empty.db");
    const std::string wal_path = TestPath("nexusdb_db_empty.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    {
      std::ofstream out(db_path, std::ios::binary | std::ios::trunc);
    }
    CHECK("placeholder file is zero-length", FileSize(db_path) == 0);
    std::unique_ptr<Database> db;
    CHECK("open over a zero-length file initializes it",
          Database::Open(db_path, wal_path, 16, &db).ok());
    CHECK("initialized database is usable",
          db != nullptr && db->executor().CreateTable("t").ok());
    if (db != nullptr) {
      db->Close();
    }
    CHECK("database file is now non-empty", FileSize(db_path) > 0);
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 11. A foreign, non-empty file is rejected, not overwritten ----
  {
    const std::string db_path = TestPath("nexusdb_db_foreign.db");
    const std::string wal_path = TestPath("nexusdb_db_foreign.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::vector<std::uint8_t> junk(Page::size() * 2, 0xEE);
    {
      std::ofstream out(db_path, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(junk.data()),
                static_cast<std::streamsize>(junk.size()));
    }
    std::unique_ptr<Database> db;
    const Status s = Database::Open(db_path, wal_path, 16, &db);
    CHECK("foreign file fails loudly (kBadState)",
          !s.ok() && s.code == DbError::kBadState);
    CHECK("foreign file was not overwritten",
          FileSize(db_path) == junk.size());
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 12. An existing database with a foreign log fails loudly, no loss ----
  {
    const std::string db_path = TestPath("nexusdb_db_badlog.db");
    const std::string wal_path = TestPath("nexusdb_db_badlog.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::uint64_t size_before = 0;
    {
      std::unique_ptr<Database> db;
      CHECK("create base database",
            Database::Open(db_path, wal_path, 16, &db).ok());
      CHECK("base table",
            db != nullptr && db->executor().CreateTable("t").ok());
      CHECK("base close", db != nullptr && db->Close().ok());
      size_before = FileSize(db_path);
    }
    {
      std::unique_ptr<Wal> wal;
      CHECK("open log for tampering", Wal::Open(wal_path, &wal).ok());
      std::vector<std::uint8_t> buf(Page::size());
      FillPage(buf.data(), 0x77, 9999);
      CHECK("append bogus image",
            wal->AppendPageImage(9999, buf.data(), buf.size()).ok());
      CHECK("flush bogus image", wal->Flush().ok());
      wal->Close();
    }
    std::unique_ptr<Database> db;
    const Status s = Database::Open(db_path, wal_path, 16, &db);
    CHECK("open with a foreign log fails loudly (kBadState)",
          !s.ok() && s.code == DbError::kBadState);
    CHECK("existing database was not clobbered",
          FileSize(db_path) == size_before);

    RemoveFile(wal_path);
    CHECK("database reopens after the foreign log is removed",
          Database::Open(db_path, wal_path, 16, &db).ok());
    CHECK("original table intact",
          db != nullptr && db->executor().TableExists("t"));
    if (db != nullptr) {
      db->Close();
    }
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
