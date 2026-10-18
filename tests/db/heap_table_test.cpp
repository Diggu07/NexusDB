// NexusDB — R1.2 heap-table test.
//
// Deterministic tests over a real database file in the OS temp directory
// (removed before/after). Covers: create/open, insert/get round trips, records
// spanning several pages, update/delete, scan order with tombstone omission,
// delete+reinsert, oversized and page-full behaviour, invalid page/slot ids and
// corrupted pages, BufferPool pinning failures, directory-chain validation
// (cycle / out-of-range / a hand-built two-page chain), and close/reopen
// persistence through the explicit FlushAll + Close workflow.
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
#include "db/heap_table.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/slotted_page.h"

namespace {

using nexusdb::BufferPool;
using nexusdb::DbError;
using nexusdb::HeapTable;
using nexusdb::Page;
using nexusdb::PageStorage;
using nexusdb::SlottedPage;
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

// Documented directory-page layout offsets (see include/db/heap_table.h).
constexpr std::size_t kDirOffVersion = 4;
constexpr std::size_t kDirOffCount = 8;
constexpr std::size_t kDirOffNext = 12;
constexpr std::size_t kDirOffIds = HeapTable::kDirectoryHeaderSize;

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

Status InsertPattern(HeapTable& table, std::size_t len, std::uint8_t seed,
                     HeapTable::RecordId* out) {
  std::vector<std::uint8_t> buf(len);
  FillPattern(buf.data(), len, seed);
  return table.Insert(buf.data(), len, out);
}

bool GetMatches(const HeapTable& table, const HeapTable::RecordId& id,
                std::size_t expect_len, std::uint8_t seed) {
  const std::uint8_t* data = nullptr;
  std::size_t len = 0;
  if (!table.Get(id, &data, &len).ok()) {
    return false;
  }
  return len == expect_len && MatchesPattern(data, len, seed);
}

struct ScanLog {
  HeapTable::RecordId ids[256];
  std::size_t count;
  std::size_t stop_after;
};

bool LogScan(const HeapTable::RecordId& id, const std::uint8_t* data,
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

void RawWriteU16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void RawWriteU32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

// Writes a directory page into `page` with magic 'NHT1', the given id count and
// next link, and the supplied data-page ids.
void BuildDirectoryPage(Page* page, const std::vector<Page::PageId>& ids,
                        Page::PageId next) {
  std::uint8_t* d = page->data();
  std::memset(d, 0, Page::size());
  d[0] = 0x4Eu;  // 'N'
  d[1] = 0x48u;  // 'H'
  d[2] = 0x54u;  // 'T'
  d[3] = 0x31u;  // '1'
  RawWriteU16(d + kDirOffVersion, HeapTable::kDirectoryVersion);
  RawWriteU32(d + kDirOffCount, static_cast<std::uint32_t>(ids.size()));
  RawWriteU32(d + kDirOffNext, next);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    RawWriteU32(d + kDirOffIds + i * HeapTable::kDirectoryIdSize, ids[i]);
  }
  page->MarkDirty();
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
  printf("=== NexusDB R1.2: heap-table test ===\n\n");

  // ---- 1. Create / open basics ----
  {
    const std::string path = TestDbPath("nexusdb_heap_basic.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    CHECK("storage opens", storage != nullptr);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    CHECK("pool created", pool != nullptr);

    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    CHECK("Create succeeds",
          HeapTable::Create(storage.get(), pool.get(), &table, &root).ok());
    CHECK("Create returns a table", table != nullptr);
    CHECK("root page id is valid",
          root != Page::kInvalidId && table->root() == root);
    CHECK("new table has no data pages", table->data_page_count() == 0);

    std::unique_ptr<HeapTable> reopened;
    CHECK("Open on the returned root succeeds",
          HeapTable::Open(storage.get(), pool.get(), root, &reopened).ok());
    CHECK("reopened empty table has no data pages",
          reopened != nullptr && reopened->data_page_count() == 0);

    std::unique_ptr<HeapTable> bad;
    CHECK("Create with null out fails",
          !HeapTable::Create(storage.get(), pool.get(), nullptr, &root).ok());
    CHECK("Open with invalid root fails",
          !HeapTable::Open(storage.get(), pool.get(), Page::kInvalidId, &bad)
               .ok());
    CHECK("Open with null pool fails",
          !HeapTable::Open(storage.get(), nullptr, root, &bad).ok());

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 2. Insert / get round trips ----
  {
    const std::string path = TestDbPath("nexusdb_heap_roundtrip.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    HeapTable::RecordId a;
    CHECK("insert first record ok", InsertPattern(*table, 5, 1, &a).ok());
    CHECK("first record id is valid and slot 0",
          a.valid() && a.page != Page::kInvalidId && a.slot == 0);
    CHECK("first record reads back", GetMatches(*table, a, 5, 1));

    HeapTable::RecordId b;
    CHECK("insert second record ok", InsertPattern(*table, 9, 2, &b).ok());
    CHECK("second record lands in the same page at slot 1",
          b.page == a.page && b.slot == 1);
    CHECK("both records read back",
          GetMatches(*table, a, 5, 1) && GetMatches(*table, b, 9, 2));
    CHECK("table has exactly one data page", table->data_page_count() == 1);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 3. Multiple records spanning several pages ----
  {
    const std::string path = TestDbPath("nexusdb_heap_multipage.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    const std::size_t kRecLen = 1000;
    std::vector<HeapTable::RecordId> ids;
    bool inserted = true;
    for (std::uint8_t i = 0; i < 10; ++i) {
      HeapTable::RecordId id;
      if (!InsertPattern(*table, kRecLen, static_cast<std::uint8_t>(i * 20 + 1),
                         &id)
               .ok()) {
        inserted = false;
        break;
      }
      ids.push_back(id);
    }
    CHECK("all 10 large records inserted", inserted && ids.size() == 10);
    CHECK("records span three data pages", table->data_page_count() == 3);

    bool all_match = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (!GetMatches(*table, ids[i], kRecLen,
                      static_cast<std::uint8_t>(i * 20 + 1))) {
        all_match = false;
      }
    }
    CHECK("every record reads back across pages", all_match);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 4. Update and delete ----
  {
    const std::string path = TestDbPath("nexusdb_heap_update.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    HeapTable::RecordId a, b, c;
    InsertPattern(*table, 6, 10, &a);
    InsertPattern(*table, 20, 20, &b);
    InsertPattern(*table, 30, 30, &c);

    std::vector<std::uint8_t> updated(10);
    FillPattern(updated.data(), updated.size(), 21);
    CHECK("grow-update succeeds",
          table->Update(b, updated.data(), updated.size()).ok());
    CHECK("updated record reads back", GetMatches(*table, b, 10, 21));
    CHECK("neighbours untouched by update",
          GetMatches(*table, a, 6, 10) && GetMatches(*table, c, 30, 30));

    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    CHECK("delete record ok", table->Delete(b).ok());
    Status gone = table->Get(b, &data, &len);
    CHECK("deleted record reads as kNotFound",
          !gone.ok() && gone.code == DbError::kNotFound);
    Status again = table->Delete(b);
    CHECK("deleting twice is kNotFound",
          !again.ok() && again.code == DbError::kNotFound);

    ScanLog log{};
    CHECK("scan succeeds after delete", table->Scan(LogScan, &log).ok());
    CHECK("scan omits the deleted record and keeps order",
          log.count == 2 && log.ids[0] == a && log.ids[1] == c);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 5. Scan order across pages ----
  {
    const std::string path = TestDbPath("nexusdb_heap_scan.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 8);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    // 900-byte records: several per page, so the scan crosses page boundaries.
    std::vector<HeapTable::RecordId> ids;
    for (std::uint8_t i = 0; i < 8; ++i) {
      HeapTable::RecordId id;
      InsertPattern(*table, 900, static_cast<std::uint8_t>(i + 1), &id);
      ids.push_back(id);
    }
    CHECK("scan test used multiple pages", table->data_page_count() >= 2);

    // Delete slots 2 and 5.
    table->Delete(ids[2]);
    table->Delete(ids[5]);

    std::vector<HeapTable::RecordId> expect;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (i != 2 && i != 5) {
        expect.push_back(ids[i]);
      }
    }
    ScanLog log{};
    table->Scan(LogScan, &log);
    bool order_ok = log.count == expect.size();
    for (std::size_t i = 0; order_ok && i < expect.size(); ++i) {
      order_ok = log.ids[i] == expect[i];
    }
    CHECK("scan yields live records in insertion order", order_ok);

    ScanLog early{};
    early.stop_after = 2;
    CHECK("scan can stop early",
          table->Scan(LogScan, &early).ok() && early.count == 2);
    CHECK("scan rejects a null callback",
          table->Scan(nullptr, &log).code == DbError::kInvalidArgument);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 6. Delete followed by reinsertion ----
  {
    const std::string path = TestDbPath("nexusdb_heap_reinsert.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    HeapTable::RecordId a, b;
    InsertPattern(*table, 10, 1, &a);
    InsertPattern(*table, 10, 2, &b);
    CHECK("delete first record", table->Delete(a).ok());

    HeapTable::RecordId d;
    CHECK("reinsert after delete succeeds",
          InsertPattern(*table, 12, 3, &d).ok());
    CHECK("reinserted record reads back", GetMatches(*table, d, 12, 3));
    ScanLog log{};
    table->Scan(LogScan, &log);
    CHECK("table holds exactly two live records after reinsert", log.count == 2);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 7. Oversized records and page-full behaviour ----
  {
    const std::string path = TestDbPath("nexusdb_heap_full.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    std::vector<std::uint8_t> big(SlottedPage::kMaxRecordSize);
    FillPattern(big.data(), big.size(), 42);
    HeapTable::RecordId full_id;
    CHECK("insert a maximum-size record",
          table->Insert(big.data(), big.size(), &full_id).ok());
    CHECK("maximum record reads back",
          GetMatches(*table, full_id, big.size(), 42));
    CHECK("the maximum record fills one page",
          table->data_page_count() == 1);

    HeapTable::RecordId small;
    CHECK("a following record spills to a new page",
          InsertPattern(*table, 5, 7, &small).ok());
    CHECK("spill created a second data page", table->data_page_count() == 2);
    CHECK("both pages readable",
          GetMatches(*table, full_id, big.size(), 42) &&
              GetMatches(*table, small, 5, 7));

    std::size_t pages_before = table->data_page_count();
    std::vector<std::uint8_t> over(SlottedPage::kMaxRecordSize + 1);
    FillPattern(over.data(), over.size(), 9);
    HeapTable::RecordId bad;
    Status over_status = table->Insert(over.data(), over.size(), &bad);
    CHECK("oversized record rejected with kBufferFull",
          !over_status.ok() && over_status.code == DbError::kBufferFull);
    CHECK("oversized insert allocated no page",
          table->data_page_count() == pages_before);
    CHECK("oversized insert returned no record id", !bad.valid());

    CHECK("empty record rejected",
          table->Insert(big.data(), 0, &bad).code == DbError::kInvalidArgument);
    CHECK("null record rejected",
          table->Insert(nullptr, 5, &bad).code == DbError::kInvalidArgument);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 8. Invalid page and slot ids, and corrupted pages ----
  {
    const std::string path = TestDbPath("nexusdb_heap_invalid.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    HeapTable::RecordId id;
    InsertPattern(*table, 8, 77, &id);

    const std::uint8_t* data = nullptr;
    std::size_t len = 0;

    HeapTable::RecordId none;
    CHECK("invalid record id rejected", !table->Get(none, &data, &len).ok());
    CHECK("Get with null out rejected",
          table->Get(id, nullptr, &len).code == DbError::kInvalidArgument);

    HeapTable::RecordId not_a_page;
    not_a_page.page = root;  // the root is a directory page, not a data page
    not_a_page.slot = 0;
    Status member = table->Get(not_a_page, &data, &len);
    CHECK("record id for a non-data page is kNotFound",
          !member.ok() && member.code == DbError::kNotFound);

    HeapTable::RecordId far;
    far.page = 999999;
    far.slot = 0;
    CHECK("record id for an unknown page is kNotFound",
          table->Get(far, &data, &len).code == DbError::kNotFound);

    HeapTable::RecordId badslot = id;
    badslot.slot = 99;
    Status slot_status = table->Get(badslot, &data, &len);
    CHECK("out-of-range slot is kInvalidArgument",
          !slot_status.ok() &&
              slot_status.code == DbError::kInvalidArgument);

    // Corrupt the data page's magic; reads must fail, not crash.
    {
      BufferPool::Handle handle;
      CHECK("fetch data page for corruption", pool->Fetch(id.page, &handle).ok());
      std::memset(handle.page().data(), 0, 4);
      handle.MarkDirty();
    }
    CHECK("Get on a corrupted page fails",
          !table->Get(id, &data, &len).ok());

    // Open(): a data page is not a directory page; a bad id is rejected.
    std::unique_ptr<HeapTable> bad;
    CHECK("Open on a non-directory page fails",
          !HeapTable::Open(storage.get(), pool.get(), id.page, &bad).ok());
    CHECK("Open on an out-of-range page fails",
          !HeapTable::Open(storage.get(), pool.get(), 1000000, &bad).ok());
    CHECK("Open with null out fails",
          !HeapTable::Open(storage.get(), pool.get(), root, nullptr).ok());

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 9. BufferPool pinning failure propagation ----
  {
    const std::string path = TestDbPath("nexusdb_heap_pinning.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 1);
    std::unique_ptr<HeapTable> table;
    Page::PageId root = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &table, &root);

    HeapTable::RecordId first;
    CHECK("first insert works with a 1-frame pool",
          InsertPattern(*table, 8, 5, &first).ok());

    // Pin the root (now the only resident frame), then force the table to fetch
    // a non-resident data page: every frame is pinned, so this must fail with
    // kBufferFull rather than evicting or corrupting anything.
    {
      BufferPool::Handle pin;
      CHECK("pin the root page", pool->Fetch(root, &pin).ok());

      HeapTable::RecordId second;
      Status s = InsertPattern(*table, 8, 6, &second);
      CHECK("insert under pinned pool is kBufferFull",
            !s.ok() && s.code == DbError::kBufferFull);
      CHECK("failed insert returned no valid id", !second.valid());
      CHECK("failed insert added no data page", table->data_page_count() == 1);
    }

    // With the pin released the original record is intact and readable.
    CHECK("existing record survives the failed insert",
          GetMatches(*table, first, 8, 5));
    ScanLog log{};
    table->Scan(LogScan, &log);
    CHECK("no phantom record was created", log.count == 1);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 10. Close/reopen persistence ----
  {
    const std::string path = TestDbPath("nexusdb_heap_persist.db");
    RemoveDb(path);

    std::vector<HeapTable::RecordId> ids;
    Page::PageId root = Page::kInvalidId;
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<HeapTable> table;
      CHECK("persist: create",
            HeapTable::Create(storage.get(), pool.get(), &table, &root).ok());
      for (std::uint8_t i = 0; i < 6; ++i) {
        HeapTable::RecordId id;
        CHECK("persist: insert",
              InsertPattern(*table, 50 + i, static_cast<std::uint8_t>(i + 1),
                            &id)
                  .ok());
        ids.push_back(id);
      }
      CHECK("persist: flush pool", pool->FlushAll().ok());
      pool.reset();
      CHECK("persist: close storage", storage->Close().ok());
    }
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<HeapTable> table;
      CHECK("persist: reopen table",
            HeapTable::Open(storage.get(), pool.get(), root, &table).ok());
      CHECK("persist: same data page count", table->data_page_count() == 1);
      bool ok = true;
      for (std::uint8_t i = 0; i < 6; ++i) {
        if (!GetMatches(*table, ids[i], 50 + i, static_cast<std::uint8_t>(i + 1))) {
          ok = false;
        }
      }
      CHECK("persist: all records survive reopen", ok);

      // Update after reopen, flush, reopen once more.
      std::vector<std::uint8_t> updated(120);
      FillPattern(updated.data(), updated.size(), 99);
      CHECK("persist: update after reopen",
            table->Update(ids[0], updated.data(), updated.size()).ok());
      CHECK("persist: flush after update", pool->FlushAll().ok());
      pool.reset();
      storage->Close();
    }
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<HeapTable> table;
      CHECK("persist: reopen again",
            HeapTable::Open(storage.get(), pool.get(), root, &table).ok());
      CHECK("persist: updated record survives second reopen",
            GetMatches(*table, ids[0], 120, 99));
      pool.reset();
      storage->Close();
    }
    RemoveDb(path);
  }

  // ---- 11. Directory-chain validation and a hand-built two-page chain ----
  {
    // 11a. A hand-built chain D1 -> D2, each owning one data page, must open
    // and both records must be reachable.
    const std::string path = TestDbPath("nexusdb_heap_chain.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);

    Page d1, d2, pd1, pd2;
    storage->AllocatePage(&d1);
    storage->AllocatePage(&d2);
    storage->AllocatePage(&pd1);
    SlottedPage::Init(&pd1);
    {
      SlottedPage sp(&pd1);
      SlottedPage::SlotId slot;
      std::uint8_t rec[10];
      FillPattern(rec, sizeof(rec), 1);
      sp.Insert(rec, sizeof(rec), &slot);
    }
    storage->WritePage(pd1);
    storage->AllocatePage(&pd2);
    SlottedPage::Init(&pd2);
    {
      SlottedPage sp(&pd2);
      SlottedPage::SlotId slot;
      std::uint8_t rec[14];
      FillPattern(rec, sizeof(rec), 2);
      sp.Insert(rec, sizeof(rec), &slot);
    }
    storage->WritePage(pd2);

    BuildDirectoryPage(&d1, {pd1.id()}, d2.id());
    storage->WritePage(d1);
    BuildDirectoryPage(&d2, {pd2.id()}, Page::kInvalidId);
    storage->WritePage(d2);

    std::unique_ptr<HeapTable> table;
    CHECK("open a hand-built two-page chain",
          HeapTable::Open(storage.get(), pool.get(), d1.id(), &table).ok());
    CHECK("chain exposes both data pages",
          table != nullptr && table->data_page_count() == 2);
    HeapTable::RecordId r1{pd1.id(), 0};
    HeapTable::RecordId r2{pd2.id(), 0};
    CHECK("records from both chain pages are reachable",
          GetMatches(*table, r1, 10, 1) && GetMatches(*table, r2, 14, 2));
    pool.reset();
    storage->Close();
    RemoveDb(path);

    // 11b. A cyclic next link is rejected, not followed forever.
    const std::string path2 = TestDbPath("nexusdb_heap_cycle.db");
    RemoveDb(path2);
    std::unique_ptr<PageStorage> storage2 = OpenStorage(path2);
    std::unique_ptr<BufferPool> pool2 = MakePool(storage2.get(), 16);
    std::unique_ptr<HeapTable> created;
    Page::PageId root2 = Page::kInvalidId;
    HeapTable::Create(storage2.get(), pool2.get(), &created, &root2);
    {
      BufferPool::Handle handle;
      pool2->Fetch(root2, &handle);
      RawWriteU32(handle.page().data() + kDirOffNext, root2);  // self-cycle
      handle.MarkDirty();
      pool2->FlushPage(root2);
    }
    std::unique_ptr<HeapTable> cyclic;
    Status cs = HeapTable::Open(storage2.get(), pool2.get(), root2, &cyclic);
    CHECK("cyclic directory chain rejected",
          !cs.ok() && cs.code == DbError::kBadState);

    // 11c. An out-of-range next link is rejected.
    {
      BufferPool::Handle handle;
      pool2->Fetch(root2, &handle);
      RawWriteU32(handle.page().data() + kDirOffNext, 0xFFFFFFF0u);
      handle.MarkDirty();
      pool2->FlushPage(root2);
    }
    std::unique_ptr<HeapTable> dangling;
    CHECK("out-of-range next link rejected",
          !HeapTable::Open(storage2.get(), pool2.get(), root2, &dangling).ok());

    pool2.reset();
    storage2->Close();
    RemoveDb(path2);
  }

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
