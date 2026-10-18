// NexusDB — R1.3 query-executor test.
//
// Deterministic tests over a real database file in the OS temp directory
// (removed before/after). Exercises the composed Catalog + HeapTable surface:
// create/open, table creation and enumeration, CRUD against named tables,
// scans spanning multiple pages with early termination, table-scoped RecordId
// isolation, invalid names/ids, persistence across reopen, and BufferPool
// pinning failures.
//
// Same plain PASS/FAIL style as the other DB tests: exit 0 on success, 1 on any
// failure. No timing assumptions.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/query_executor.h"

namespace {

using nexusdb::BufferPool;
using nexusdb::DbError;
using nexusdb::Page;
using nexusdb::PageStorage;
using nexusdb::QueryExecutor;
using nexusdb::Status;

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

std::string TestDbPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void RemoveDb(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

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

Status InsertPattern(QueryExecutor& ex, const std::string& table,
                     std::size_t len, std::uint8_t seed,
                     QueryExecutor::RecordId* out) {
  std::vector<std::uint8_t> buf(len);
  FillPattern(buf.data(), len, seed);
  return ex.Insert(table, buf.data(), len, out);
}

bool GetMatches(QueryExecutor& ex, const std::string& table,
                const QueryExecutor::RecordId& id, std::size_t expect_len,
                std::uint8_t seed) {
  const std::uint8_t* data = nullptr;
  std::size_t len = 0;
  if (!ex.Get(table, id, &data, &len).ok()) {
    return false;
  }
  return len == expect_len && MatchesPattern(data, len, seed);
}

struct ScanLog {
  QueryExecutor::RecordId ids[256];
  std::size_t count;
  std::size_t stop_after;
};

bool LogScan(const QueryExecutor::RecordId& id, const std::uint8_t* data,
             std::size_t len, void* ctx) {
  ScanLog* log = static_cast<ScanLog*>(ctx);
  if (log->count < 256) {
    log->ids[log->count] = id;
  }
  ++log->count;
  if (log->stop_after != 0 && log->count >= log->stop_after) {
    return false;
  }
  (void)data;
  (void)len;
  return true;
}

std::unique_ptr<PageStorage> OpenStorage(const std::string& path) {
  std::unique_ptr<PageStorage> storage;
  if (!PageStorage::Open(path, &storage).ok()) {
    return nullptr;
  }
  return storage;
}

std::unique_ptr<BufferPool> MakePool(PageStorage* storage, std::size_t frames) {
  std::unique_ptr<BufferPool> pool;
  if (!BufferPool::Create(storage, frames, &pool).ok()) {
    return nullptr;
  }
  return pool;
}

}  // namespace

int main() {
  printf("=== NexusDB R1.3: query-executor test ===\n\n");

  // ---- 1. Create / open basics ----
  {
    const std::string path = TestDbPath("nexusdb_qe_basic.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    CHECK("storage opens", storage != nullptr);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    CHECK("pool created", pool != nullptr);

    std::unique_ptr<QueryExecutor> ex;
    Page::PageId root = Page::kInvalidId;
    CHECK("Create succeeds",
          QueryExecutor::Create(storage.get(), pool.get(), &ex, &root).ok());
    CHECK("Create returns an executor", ex != nullptr);
    CHECK("catalog root is valid and reported",
          root != Page::kInvalidId && ex->catalog_root() == root);
    CHECK("fresh executor has no tables", ex->table_count() == 0);
    CHECK("fresh executor reports no table exists",
          !ex->TableExists("users"));

    std::unique_ptr<QueryExecutor> reopened;
    CHECK("Open on the returned root succeeds",
          QueryExecutor::Open(storage.get(), pool.get(), root, &reopened).ok());
    CHECK("reopened executor has no tables",
          reopened != nullptr && reopened->table_count() == 0);

    std::unique_ptr<QueryExecutor> bad;
    CHECK("Create with null out fails",
          !QueryExecutor::Create(storage.get(), pool.get(), nullptr, &root)
               .ok());
    CHECK("Open with invalid root fails",
          !QueryExecutor::Open(storage.get(), pool.get(), Page::kInvalidId, &bad)
               .ok());

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 2. Table creation and enumeration ----
  {
    const std::string path = TestDbPath("nexusdb_qe_tables.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<QueryExecutor> ex;
    QueryExecutor::Create(storage.get(), pool.get(), &ex, nullptr);

    CHECK("create 'users'", ex->CreateTable("users").ok());
    CHECK("create 'posts_2026'", ex->CreateTable("posts_2026").ok());
    CHECK("TableExists is true for created tables",
          ex->TableExists("users") && ex->TableExists("posts_2026"));
    CHECK("TableExists is false for others", !ex->TableExists("comments"));
    CHECK("table count is two", ex->table_count() == 2);
    CHECK("duplicate create rejected",
          ex->CreateTable("users").code == DbError::kInvalidArgument);
    CHECK("invalid name rejected",
          ex->CreateTable("bad name").code == DbError::kInvalidArgument);

    std::vector<std::string> listed;
    CHECK("ListTables succeeds", ex->ListTables(&listed).ok());
    CHECK("ListTables returns both in order",
          listed.size() == 2 && listed[0] == "users" &&
              listed[1] == "posts_2026");
    CHECK("ListTables null out rejected",
          ex->ListTables(nullptr).code == DbError::kInvalidArgument);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 3. CRUD against a named table ----
  {
    const std::string path = TestDbPath("nexusdb_qe_crud.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<QueryExecutor> ex;
    QueryExecutor::Create(storage.get(), pool.get(), &ex, nullptr);
    ex->CreateTable("t");

    QueryExecutor::RecordId a, b;
    CHECK("insert first record", InsertPattern(*ex, "t", 6, 10, &a).ok());
    CHECK("insert second record", InsertPattern(*ex, "t", 12, 20, &b).ok());
    CHECK("first record reads back", GetMatches(*ex, "t", a, 6, 10));
    CHECK("second record reads back", GetMatches(*ex, "t", b, 12, 20));

    std::vector<std::uint8_t> updated(9);
    FillPattern(updated.data(), updated.size(), 77);
    CHECK("update record", ex->Update("t", a, updated.data(), updated.size()).ok());
    CHECK("updated record reads back", GetMatches(*ex, "t", a, 9, 77));
    CHECK("neighbour untouched by update", GetMatches(*ex, "t", b, 12, 20));

    CHECK("delete record", ex->Delete("t", b).ok());
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    Status gone = ex->Get("t", b, &data, &len);
    CHECK("deleted record reads as kNotFound",
          !gone.ok() && gone.code == DbError::kNotFound);
    CHECK("delete of a missing record is kNotFound",
          ex->Delete("t", b).code == DbError::kNotFound);

    ScanLog log{};
    CHECK("scan after delete succeeds", ex->Scan("t", LogScan, &log).ok());
    CHECK("scan yields only the live record", log.count == 1 && log.ids[0] == a);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 4. Multi-page scans and early termination ----
  {
    const std::string path = TestDbPath("nexusdb_qe_scan.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 8);
    std::unique_ptr<QueryExecutor> ex;
    QueryExecutor::Create(storage.get(), pool.get(), &ex, nullptr);
    ex->CreateTable("wide");

    std::vector<QueryExecutor::RecordId> ids;
    for (std::uint8_t i = 1; i <= 8; ++i) {
      QueryExecutor::RecordId id;
      InsertPattern(*ex, "wide", 900, i, &id);
      ids.push_back(id);
    }
    // Records must have spread over at least two pages for this to be a
    // meaningful multi-page scan.
    bool multipage = ids[0].page != ids[7].page;
    CHECK("records span multiple pages", multipage);

    ex->Delete("wide", ids[2]);
    ex->Delete("wide", ids[5]);

    ScanLog log{};
    ex->Scan("wide", LogScan, &log);
    bool order_ok = log.count == 6;
    std::size_t k = 0;
    for (std::size_t i = 0; i < ids.size() && order_ok; ++i) {
      if (i == 2 || i == 5) {
        continue;
      }
      order_ok = log.ids[k++] == ids[i];
    }
    CHECK("scan yields live records in order across pages", order_ok);

    ScanLog early{};
    early.stop_after = 3;
    CHECK("scan stops early",
          ex->Scan("wide", LogScan, &early).ok() && early.count == 3);
    CHECK("scan rejects a null callback",
          ex->Scan("wide", nullptr, &log).code == DbError::kInvalidArgument);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 5. RecordIds are table-scoped ----
  {
    const std::string path = TestDbPath("nexusdb_qe_scope.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<QueryExecutor> ex;
    QueryExecutor::Create(storage.get(), pool.get(), &ex, nullptr);
    ex->CreateTable("left");
    ex->CreateTable("right");

    QueryExecutor::RecordId left_id, right_id;
    InsertPattern(*ex, "left", 8, 1, &left_id);
    InsertPattern(*ex, "right", 8, 2, &right_id);
    CHECK("both tables got valid ids", left_id.valid() && right_id.valid());

    CHECK("a record reads from its own table",
          GetMatches(*ex, "left", left_id, 8, 1) &&
              GetMatches(*ex, "right", right_id, 8, 2));

    // A page id from one table must not resolve in the other.
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    Status crossed = ex->Get("right", left_id, &data, &len);
    CHECK("a record id from another table is kNotFound",
          !crossed.ok() && crossed.code == DbError::kNotFound);
    CHECK("cross-table update is kNotFound",
          ex->Update("right", left_id, reinterpret_cast<const std::uint8_t*>("x"),
                     1)
                  .code == DbError::kNotFound);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 6. Invalid names, missing tables, and invalid ids ----
  {
    const std::string path = TestDbPath("nexusdb_qe_invalid.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<QueryExecutor> ex;
    QueryExecutor::Create(storage.get(), pool.get(), &ex, nullptr);
    ex->CreateTable("real");

    const std::uint8_t payload[4] = {1, 2, 3, 4};
    QueryExecutor::RecordId out;
    Status ins = ex->Insert("missing", payload, sizeof(payload), &out);
    CHECK("insert into a missing table is kNotFound",
          !ins.ok() && ins.code == DbError::kNotFound);

    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    QueryExecutor::RecordId none;
    CHECK("get with an invalid id fails",
          !ex->Get("real", none, &data, &len).ok());
    CHECK("insert of an empty record fails",
          ex->Insert("real", payload, 0, &out).code == DbError::kInvalidArgument);
    CHECK("insert of a null record fails",
          ex->Insert("real", nullptr, 4, &out).code == DbError::kInvalidArgument);

    QueryExecutor::RecordId good;
    InsertPattern(*ex, "real", 4, 5, &good);
    QueryExecutor::RecordId far;
    far.page = 999999;
    far.slot = 0;
    CHECK("record id for an unknown page is kNotFound",
          ex->Get("real", far, &data, &len).code == DbError::kNotFound);
    QueryExecutor::RecordId bad_slot = good;
    bad_slot.slot = 500;
    CHECK("out-of-range slot is kInvalidArgument",
          ex->Get("real", bad_slot, &data, &len).code ==
              DbError::kInvalidArgument);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 7. Persistence across close/reopen ----
  {
    const std::string path = TestDbPath("nexusdb_qe_persist.db");
    RemoveDb(path);
    Page::PageId root = Page::kInvalidId;
    QueryExecutor::RecordId saved;

    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<QueryExecutor> ex;
      CHECK("persist: create executor",
            QueryExecutor::Create(storage.get(), pool.get(), &ex, &root).ok());
      CHECK("persist: create table",
            ex->CreateTable("events").ok());
      CHECK("persist: insert record",
            InsertPattern(*ex, "events", 16, 33, &saved).ok());
      CHECK("persist: flush", pool->FlushAll().ok());
      pool.reset();
      CHECK("persist: close", storage->Close().ok());
    }
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<QueryExecutor> ex;
      CHECK("reopen: executor reopens",
            QueryExecutor::Open(storage.get(), pool.get(), root, &ex).ok());
      CHECK("reopen: table present",
            ex->table_count() == 1 && ex->TableExists("events"));
      CHECK("reopen: record survives with its bytes",
            GetMatches(*ex, "events", saved, 16, 33));

      // Mutate after reopen, flush, reopen once more.
      std::vector<std::uint8_t> updated(24);
      FillPattern(updated.data(), updated.size(), 90);
      CHECK("reopen: update", ex->Update("events", saved, updated.data(),
                                         updated.size())
                                  .ok());
      CHECK("reopen: flush", pool->FlushAll().ok());
      pool.reset();
      storage->Close();
    }
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<QueryExecutor> ex;
      CHECK("reopen again: executor opens",
            QueryExecutor::Open(storage.get(), pool.get(), root, &ex).ok());
      CHECK("reopen again: update survives",
            GetMatches(*ex, "events", saved, 24, 90));
      pool.reset();
      storage->Close();
    }
    RemoveDb(path);
  }

  // ---- 8. BufferPool pinning failure propagation ----
  {
    const std::string path = TestDbPath("nexusdb_qe_pinning.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 2);
    std::unique_ptr<QueryExecutor> ex;
    Page::PageId root = Page::kInvalidId;
    QueryExecutor::Create(storage.get(), pool.get(), &ex, &root);
    ex->CreateTable("t");
    QueryExecutor::RecordId first;
    CHECK("pinning: first insert works",
          InsertPattern(*ex, "t", 8, 5, &first).ok());

    // Pin every frame, then insert into a new page: the table's directory page
    // and the new data page cannot be loaded, so the insert must fail cleanly.
    {
      BufferPool::Handle p1, p2;
      pool->Fetch(root, &p1);
      pool->Fetch(first.page, &p2);
      QueryExecutor::RecordId second;
      Status s = InsertPattern(*ex, "t", 8, 6, &second);
      CHECK("insert with a fully pinned pool is kBufferFull",
            !s.ok() && s.code == DbError::kBufferFull);
      CHECK("failed insert returned no valid id", !second.valid());
    }

    CHECK("existing record survives the failed insert",
          GetMatches(*ex, "t", first, 8, 5));
    ScanLog log{};
    ex->Scan("t", LogScan, &log);
    CHECK("no phantom record created", log.count == 1);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
