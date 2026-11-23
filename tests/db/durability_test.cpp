// NexusDB — durability regression test.
//
// Verifies the AUD-01 remediation: WAL appends and database page writes are
// synchronized to storage (fsync on Linux) before being reported durable, so
// an acknowledged modification survives an abrupt process termination after
// the synchronization calls have completed.
//
// Same plain PASS/FAIL style as the other DB tests: exit 0 on success, 1 on
// any failure. Uses disposable temporary database directories; removes them
// before and after. No destructive operations against real NexusDB data.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/durable.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/database.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/query_executor.h"
#include "db/wal.h"

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
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
using nexusdb::FsyncFile;
using nexusdb::Page;
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

std::unique_ptr<PageStorage> OpenStorage(const std::string& path) {
  std::unique_ptr<PageStorage> storage;
  if (!PageStorage::Open(path, &storage).ok()) {
    return nullptr;
  }
  return storage;
}

std::unique_ptr<Wal> OpenWal(const std::string& path) {
  std::unique_ptr<Wal> wal;
  if (!Wal::Open(path, &wal).ok()) {
    return nullptr;
  }
  return wal;
}

bool AppendImage(const std::string& wal_path, Page::PageId id,
                 std::uint8_t seed, std::unique_ptr<Wal>* wal_out = nullptr) {
  std::unique_ptr<Wal> wal = OpenWal(wal_path);
  if (wal == nullptr) {
    return false;
  }
  std::vector<std::uint8_t> buf(Page::size());
  FillPage(buf.data(), seed, id);
  if (!wal->AppendPageImage(id, buf.data(), buf.size()).ok()) {
    return false;
  }
  if (!wal->Flush().ok()) {
    return false;
  }
  if (wal_out != nullptr) {
    *wal_out = std::move(wal);
  }
  return true;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  try {
  printf("=== NexusDB: durability regression test (AUD-01) ===\n\n");

  // ---- 1. A successful durable WAL append ----
  {
    const std::string wal_path = TestPath("nexusdb_dur_wal_append.wal");
    RemoveFile(wal_path);
    std::unique_ptr<Wal> wal;
    CHECK("Open creates a log",
          Wal::Open(wal_path, &wal).ok() && wal != nullptr);
    std::vector<std::uint8_t> buf(Page::size());
    FillPage(buf.data(), 0x10, 3);
    std::uint64_t lsn = 0;
    CHECK("append returns lsn 1 and succeeds",
          wal->AppendPageImage(3, buf.data(), buf.size(), &lsn).ok() && lsn == 1);
    CHECK("nothing durable before Flush", wal->durable_lsn() == 0);
    CHECK("Flush succeeds and synchronizes", wal->Flush().ok());
    CHECK("durable lsn is 1 after Flush", wal->durable_lsn() == 1);
    CHECK("next lsn advanced to 2", wal->next_lsn() == 2);
    wal->Close();
    RemoveFile(wal_path);
  }

  // ---- 2. A successful durable database page write ----
  {
    const std::string db_path = TestPath("nexusdb_dur_page.db");
    RemoveFile(db_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    CHECK("storage opens", storage != nullptr);
    Page page;
    CHECK("allocate page", storage->AllocatePage(&page).ok());
    FillPage(page.data(), 0x20, page.id());
    CHECK("write page succeeds", storage->WritePage(page).ok());
    CHECK("flush succeeds", storage->Flush().ok());
    storage->Close();

    // Reopen and verify the page survived on disk.
    std::unique_ptr<PageStorage> reader = OpenStorage(db_path);
    CHECK("reopen storage", reader != nullptr);
    std::vector<std::uint8_t> got;
    CHECK("written page is readable back",
          reader != nullptr && ReadPageBytes(reader.get(), page.id(), &got) &&
              PageMatches(got.data(), 0x20, page.id()));
    reader->Close();
    RemoveFile(db_path);
  }

  // ---- 3 & 4. A record surviving SIGKILL after sync, then reopen ----
#if NEXUS_HAVE_FORK
  {
    const std::string db_path = TestPath("nexusdb_dur_crash.db");
    const std::string wal_path = TestPath("nexusdb_dur_crash.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);

    std::vector<std::uint8_t> rec(32);
    for (std::size_t i = 0; i < rec.size(); ++i) {
      rec[i] = static_cast<std::uint8_t>(0x70 + i);
    }

    {
      std::unique_ptr<Database> db;
      CHECK("open fresh", Database::Open(db_path, wal_path, 64, &db).ok());
      CHECK("create table", db->executor().CreateTable("survive").ok());
      CHECK("clean close", db->Close().ok());
    }

    int fds[2];
    if (::pipe(fds) != 0) {
      CHECK("crash-test pipe created", false);
    } else {
      const pid_t pid = fork();
      CHECK("fork succeeded", pid >= 0);
      if (pid == 0) {
        ::close(fds[0]);
        bool synced = false;
        do {
          std::unique_ptr<Database> db;
          if (!Database::Open(db_path, wal_path, 64, &db).ok())
            break;
          QueryExecutor::RecordId crash_id;
          if (!db->executor()
                   .Insert("survive", rec.data(), rec.size(), &crash_id)
                   .ok())
            break;
          if (!db->pool()->FlushAll().ok())
            break;
          unsigned char bytes[sizeof(crash_id.page) + sizeof(crash_id.slot)];
          std::memcpy(bytes, &crash_id.page, sizeof(crash_id.page));
          std::memcpy(bytes + sizeof(crash_id.page), &crash_id.slot,
                      sizeof(crash_id.slot));
          std::size_t written = 0;
          while (written < sizeof(bytes)) {
            const ssize_t n = ::write(fds[1], bytes + written,
                                      sizeof(bytes) - written);
            if (n <= 0) {
              if (n < 0 && errno == EINTR)
                continue;
              break;
            }
            written += static_cast<std::size_t>(n);
          }
          synced = written == sizeof(bytes);
        } while (false);
        if (!synced)
          _exit(2);
        ::kill(::getpid(), SIGKILL);
        _exit(3);
      }

      ::close(fds[1]);
      unsigned char bytes[sizeof(Page::PageId) + sizeof(std::uint16_t)];
      std::size_t got = 0;
      while (got < sizeof(bytes)) {
        const ssize_t n = ::read(fds[0], bytes + got, sizeof(bytes) - got);
        if (n == 0)
          break;
        if (n < 0) {
          if (errno == EINTR)
            continue;
          break;
        }
        got += static_cast<std::size_t>(n);
      }
      ::close(fds[0]);
      int status = 0;
      ::waitpid(pid, &status, 0);
      CHECK("child died by SIGKILL after durable flush",
            WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
      CHECK("child reported the inserted record id", got == sizeof(bytes));

      if (got == sizeof(bytes)) {
        QueryExecutor::RecordId id;
        std::memcpy(&id.page, bytes, sizeof(id.page));
        std::memcpy(&id.slot, bytes + sizeof(id.page), sizeof(id.slot));
        std::unique_ptr<Database> db;
        CHECK("reopen recovers from the log",
              Database::Open(db_path, wal_path, 64, &db).ok());
        CHECK("table survived recovery",
              db != nullptr && db->executor().TableExists("survive"));
        const std::uint8_t* data = nullptr;
        std::size_t len = 0;
        CHECK("inserted record survives SIGKILL with its bytes",
              db != nullptr &&
                  db->executor().Get("survive", id, &data, &len).ok() &&
                  len == 32 && memcmp(data, rec.data(), 32) == 0);
        if (db != nullptr)
          db->Close();
      }
      RemoveFile(db_path);
      RemoveFile(wal_path);
    }
  }
#else
  printf("SKIP: fork-based crash test requires a POSIX platform\n");
#endif

  // ---- 5. Repeated recovery without duplicating or corrupting data ----
  {
    const std::string db_path = TestPath("nexusdb_dur_repeat.db");
    const std::string wal_path = TestPath("nexusdb_dur_repeat.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    QueryExecutor::RecordId id;
    std::vector<std::uint8_t> rec(16);
    for (std::size_t i = 0; i < rec.size(); ++i) {
      rec[i] = static_cast<std::uint8_t>(0x80 + i);
    }
    {
      std::unique_ptr<Database> db;
      CHECK("open fresh", Database::Open(db_path, wal_path, 16, &db).ok());
      CHECK("create table", db->executor().CreateTable("repeat").ok());
      CHECK("insert",
            db->executor().Insert("repeat", rec.data(), rec.size(), &id).ok());
      CHECK("flush durably", db->pool()->FlushAll().ok());
      CHECK("clean close", db->Close().ok());
    }
    // Reopen three times; content must be identical each time.
    for (int round = 0; round < 3; ++round) {
      std::unique_ptr<Database> db;
      CHECK("reopen round", Database::Open(db_path, wal_path, 16, &db).ok());
      CHECK("table present", db != nullptr &&
                                 db->executor().TableExists("repeat"));
      const std::uint8_t* data = nullptr;
      std::size_t len = 0;
      CHECK("record bytes stable across reopen",
            db != nullptr &&
                db->executor().Get("repeat", id, &data, &len).ok() &&
                len == 16 && memcmp(data, rec.data(), 16) == 0);
      db->Close();
    }
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 6. WAL corruption behavior unchanged ----
  {
    const std::string wal_path = TestPath("nexusdb_dur_crc.wal");
    RemoveFile(wal_path);
    AppendImage(wal_path, 1, 0x55);
    std::ifstream in(wal_path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    in.close();
    bytes[Wal::kHeaderSize + 10] ^= 0xFFu;  // flip a payload byte
    std::ofstream out(wal_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    std::unique_ptr<Wal> wal;
    Status s = Wal::Open(wal_path, &wal);
    CHECK("checksum mismatch still rejected as kBadState",
          !s.ok() && s.code == DbError::kBadState);
    RemoveFile(wal_path);
  }

  // ---- 7. Truncated-tail behavior unchanged ----
  {
    const std::string db_path = TestPath("nexusdb_dur_trunc.db");
    const std::string wal_path = TestPath("nexusdb_dur_trunc.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1, p2;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);
    storage->AllocatePage(&p2);
    AppendImage(wal_path, 1, 0x0a);
    AppendImage(wal_path, 2, 0x0b);
    AppendImage(wal_path, 2, 0x0c);  // will be truncated
    std::error_code ec;
    std::filesystem::resize_file(wal_path, 2 * kRec + Wal::kHeaderSize + 100, ec);
    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    CHECK("Open tolerates a truncated trailing record", wal != nullptr);
    CHECK("Open recovered the two complete records",
          wal != nullptr && wal->next_lsn() == 3);
    Wal::RecoveryStats stats;
    CHECK("recover succeeds past the tail",
          wal->Recover(storage.get(), &stats).ok());
    CHECK("only the complete records were applied", stats.records_applied == 2);
    CHECK("the truncated tail is reported", stats.truncated_tail);
    wal->Close();
    storage->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 8. Synchronization failures are propagated correctly ----
  {
    // Direct seam: FsyncFile on a path that cannot be opened must fail loudly
    // with kIoError, never report success.
    const std::string missing =
        TestPath("nexusdb_dur_nonexistent_sync_target");
    RemoveFile(missing);
    Status s = FsyncFile(missing);
    CHECK("FsyncFile on a missing path returns kIoError",
          !s.ok() && s.code == DbError::kIoError);
    CHECK("FsyncFile on an empty path returns kInvalidArgument",
          FsyncFile("").code == DbError::kInvalidArgument);
  }

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    fprintf(stderr, "EXCEPTION: %s\n", e.what());
    return 1;
  } catch (...) {
    fprintf(stderr, "UNKNOWN EXCEPTION\n");
    return 1;
  }
}
