// NexusDB — R1.3 catalog test.
//
// Deterministic tests over a real database file in the OS temp directory
// (removed before/after). Covers: create/open, empty catalog, creating many
// tables, duplicate and invalid names, missing-table lookup, name enumeration,
// persistence across close/reopen, corruption and invalid references (bad
// magic/version/count, cyclic/out-of-range chain, duplicate names, missing or
// wrong-type table pages), a hand-built two-page catalog chain, and
// BufferPool pinning failures.
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
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/catalog.h"
#include "db/heap_table.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/slotted_page.h"

namespace {

using nexusdb::BufferPool;
using nexusdb::Catalog;
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

// Documented catalog-page layout offsets (see include/db/catalog.h).
constexpr std::size_t kCatOffVersion = 4;
constexpr std::size_t kCatOffCount = 8;
constexpr std::size_t kCatOffNext = 12;
constexpr std::size_t kCatOffEntries = Catalog::kHeaderSize;

std::string TestDbPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void RemoveDb(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
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

// Writes a catalog page into `page` with magic 'NCA1', the given entries and
// next link. `entries` are (name, table root) pairs.
void BuildCatalogPage(Page* page,
                      const std::vector<std::pair<std::string, Page::PageId>>&
                          entries,
                      Page::PageId next) {
  std::uint8_t* d = page->data();
  std::memset(d, 0, Page::size());
  d[0] = 0x4Eu;  // 'N'
  d[1] = 0x43u;  // 'C'
  d[2] = 0x41u;  // 'A'
  d[3] = 0x31u;  // '1'
  RawWriteU16(d + kCatOffVersion, Catalog::kVersion);
  RawWriteU32(d + kCatOffCount,
              static_cast<std::uint32_t>(entries.size()));
  RawWriteU32(d + kCatOffNext, next);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    std::uint8_t* e = d + kCatOffEntries + i * Catalog::kEntrySize;
    std::memset(e, 0, Catalog::kNameFieldSize);
    std::memcpy(e, entries[i].first.data(), entries[i].first.size());
    RawWriteU32(e + Catalog::kNameFieldSize, entries[i].second);
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

// Creates a fresh catalog page at `path` and returns its root id (or
// kInvalidId on failure). Storage/pool are closed before returning.
Page::PageId MakeCatalogFile(const std::string& path) {
  std::unique_ptr<PageStorage> storage = OpenStorage(path);
  if (storage == nullptr) {
    return Page::kInvalidId;
  }
  std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
  std::unique_ptr<Catalog> catalog;
  Page::PageId root = Page::kInvalidId;
  if (!Catalog::Create(storage.get(), pool.get(), &catalog, &root).ok()) {
    return Page::kInvalidId;
  }
  pool->FlushAll();
  pool.reset();
  storage->Close();
  return root;
}

}  // namespace

int main() {
  printf("=== NexusDB R1.3: catalog test ===\n\n");

  // ---- 1. Create / open basics ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_basic.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    CHECK("storage opens", storage != nullptr);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    CHECK("pool created", pool != nullptr);

    std::unique_ptr<Catalog> catalog;
    Page::PageId root = Page::kInvalidId;
    CHECK("Create succeeds",
          Catalog::Create(storage.get(), pool.get(), &catalog, &root).ok());
    CHECK("Create returns a catalog", catalog != nullptr);
    CHECK("root page id is valid",
          root != Page::kInvalidId && catalog->root() == root);
    CHECK("a fresh catalog is empty", catalog->table_count() == 0);
    CHECK("no table exists yet", !catalog->Exists("anything"));

    std::unique_ptr<Catalog> reopened;
    CHECK("Open on the returned root succeeds",
          Catalog::Open(storage.get(), pool.get(), root, &reopened).ok());
    CHECK("reopened empty catalog is empty",
          reopened != nullptr && reopened->table_count() == 0);

    std::unique_ptr<Catalog> bad;
    CHECK("Create with null storage fails",
          !Catalog::Create(nullptr, pool.get(), &bad, &root).ok());
    CHECK("Create with null pool fails",
          !Catalog::Create(storage.get(), nullptr, &bad, &root).ok());
    CHECK("Create with null out fails",
          !Catalog::Create(storage.get(), pool.get(), nullptr, &root).ok());
    CHECK("Open with invalid root fails",
          !Catalog::Open(storage.get(), pool.get(), Page::kInvalidId, &bad)
               .ok());
    CHECK("Open with null out fails",
          !Catalog::Open(storage.get(), pool.get(), root, nullptr).ok());

    CHECK("FindTable with null out fails",
          catalog->FindTable("x", nullptr).code == DbError::kInvalidArgument);
    CHECK("ListTables with null out fails",
          catalog->ListTables(nullptr).code == DbError::kInvalidArgument);
    CHECK("OpenTable with null out fails",
          catalog->OpenTable("x", nullptr).code == DbError::kInvalidArgument);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 2. Create multiple distinct tables ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_multi.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<Catalog> catalog;
    Page::PageId root = Page::kInvalidId;
    Catalog::Create(storage.get(), pool.get(), &catalog, &root);

    const char* names[] = {"alpha", "beta_2", "Gamma9"};
    std::vector<Page::PageId> roots;
    bool created = true;
    for (const char* n : names) {
      Page::PageId tr = Page::kInvalidId;
      if (!catalog->CreateTable(n, &tr).ok()) {
        created = false;
      }
      roots.push_back(tr);
    }
    CHECK("three tables created", created && roots.size() == 3);
    CHECK("catalog reports three tables", catalog->table_count() == 3);
    CHECK("all three exist",
          catalog->Exists("alpha") && catalog->Exists("beta_2") &&
              catalog->Exists("Gamma9"));

    bool distinct = roots[0] != Page::kInvalidId &&
                    roots[0] != roots[1] && roots[1] != roots[2] &&
                    roots[0] != roots[2];
    CHECK("table roots are distinct", distinct);
    for (std::size_t i = 0; i < roots.size(); ++i) {
      Page::PageId found = Page::kInvalidId;
      CHECK("FindTable returns the recorded root",
            catalog->FindTable(names[i], &found).ok() && found == roots[i]);
    }

    std::vector<std::string> listed;
    CHECK("ListTables succeeds", catalog->ListTables(&listed).ok());
    bool order_ok = listed.size() == 3;
    for (std::size_t i = 0; order_ok && i < 3; ++i) {
      order_ok = listed[i] == names[i];
    }
    CHECK("ListTables preserves creation order", order_ok);

    std::unique_ptr<HeapTable> table;
    CHECK("OpenTable returns an empty table",
          catalog->OpenTable("beta_2", &table).ok() && table != nullptr &&
              table->data_page_count() == 0 &&
              table->root() == roots[1]);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 3. Duplicate and invalid table names ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_names.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<Catalog> catalog;
    Page::PageId root = Page::kInvalidId;
    Catalog::Create(storage.get(), pool.get(), &catalog, &root);

    Page::PageId tr = Page::kInvalidId;
    CHECK("first create of 'users' succeeds", catalog->CreateTable("users", &tr).ok());
    Page::PageId dup = Page::kInvalidId;
    Status dup_status = catalog->CreateTable("users", &dup);
    CHECK("duplicate table name rejected as kInvalidArgument",
          !dup_status.ok() && dup_status.code == DbError::kInvalidArgument);
    CHECK("duplicate create added no table", catalog->table_count() == 1);
    CHECK("duplicate create returned no root", dup == Page::kInvalidId);

    const char* bad_names[] = {"", "has space", "has-dash", "dot.name",
                               "slash/name", "way_too_long_a_table_name_123456"};
    bool all_rejected = true;
    for (const char* n : bad_names) {
      Page::PageId r = Page::kInvalidId;
      Status s = catalog->CreateTable(n, &r);
      if (s.ok() || r != Page::kInvalidId) {
        all_rejected = false;
      }
    }
    CHECK("invalid table names rejected", all_rejected);
    CHECK("invalid names added nothing", catalog->table_count() == 1);

    CHECK("IsValidTableName accepts good names",
          Catalog::IsValidTableName("a") &&
              Catalog::IsValidTableName("T_1234567890"));
    CHECK("IsValidTableName rejects bad names",
          !Catalog::IsValidTableName("") &&
              !Catalog::IsValidTableName("bad name") &&
              !Catalog::IsValidTableName(std::string(32, 'x')));

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 4. Lookup of existing and missing tables ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_lookup.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
    std::unique_ptr<Catalog> catalog;
    Page::PageId root = Page::kInvalidId;
    Catalog::Create(storage.get(), pool.get(), &catalog, &root);
    catalog->CreateTable("present", nullptr);

    Page::PageId missing = Page::kInvalidId;
    Status fs = catalog->FindTable("absent", &missing);
    CHECK("FindTable of a missing table is kNotFound",
          !fs.ok() && fs.code == DbError::kNotFound);
    CHECK("FindTable leaves out as invalid on miss",
          missing == Page::kInvalidId);

    std::unique_ptr<HeapTable> table;
    Status os = catalog->OpenTable("absent", &table);
    CHECK("OpenTable of a missing table is kNotFound",
          !os.ok() && os.code == DbError::kNotFound && table == nullptr);

    CHECK("Exists is false for a missing table", !catalog->Exists("absent"));
    CHECK("Exists is true for a present table", catalog->Exists("present"));

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 5. Persistence across close/reopen (tables AND their data) ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_persist.db");
    RemoveDb(path);
    Page::PageId root = Page::kInvalidId;
    std::vector<Page::PageId> table_roots;
    HeapTable::RecordId rec_id;

    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<Catalog> catalog;
      CHECK("persist: create catalog",
            Catalog::Create(storage.get(), pool.get(), &catalog, &root).ok());
      Page::PageId tr = Page::kInvalidId;
      CHECK("persist: create 'orders'",
            catalog->CreateTable("orders", &tr).ok());
      table_roots.push_back(tr);
      CHECK("persist: create 'items'",
            catalog->CreateTable("items", nullptr).ok());

      std::unique_ptr<HeapTable> orders;
      CHECK("persist: open 'orders'",
            catalog->OpenTable("orders", &orders).ok());
      const std::uint8_t payload[6] = {10, 20, 30, 40, 50, 60};
      CHECK("persist: insert into 'orders'",
            orders->Insert(payload, sizeof(payload), &rec_id).ok());
      CHECK("persist: flush", pool->FlushAll().ok());
      pool.reset();
      CHECK("persist: close", storage->Close().ok());
    }
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<Catalog> catalog;
      CHECK("reopen: catalog reopens",
            Catalog::Open(storage.get(), pool.get(), root, &catalog).ok());
      CHECK("reopen: both tables present",
            catalog->table_count() == 2 && catalog->Exists("orders") &&
                catalog->Exists("items"));

      Page::PageId found = Page::kInvalidId;
      CHECK("reopen: table root is stable",
            catalog->FindTable("orders", &found).ok() &&
                found == table_roots[0]);

      std::unique_ptr<HeapTable> orders;
      CHECK("reopen: open 'orders'",
            catalog->OpenTable("orders", &orders).ok());
      const std::uint8_t* data = nullptr;
      std::size_t len = 0;
      bool ok = orders != nullptr && orders->Get(rec_id, &data, &len).ok() &&
                len == 6 && data != nullptr;
      bool bytes_ok = ok;
      for (std::size_t i = 0; bytes_ok && i < 6; ++i) {
        bytes_ok = data[i] == static_cast<std::uint8_t>(10 * (i + 1));
      }
      CHECK("reopen: record and its bytes survive", bytes_ok);

      pool.reset();
      storage->Close();
    }
    RemoveDb(path);
  }

  // ---- 6. Corruption and invalid references ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_corrupt.db");
    RemoveDb(path);
    Page::PageId root = MakeCatalogFile(path);
    CHECK("corruption: catalog file created", root != Page::kInvalidId);

    // 6a. Open on a non-catalog page (a real heap table root).
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      std::unique_ptr<HeapTable> table;
      Page::PageId heap_root = Page::kInvalidId;
      CHECK("corruption: make a heap table",
            HeapTable::Create(storage.get(), pool.get(), &table, &heap_root)
                .ok());
      std::unique_ptr<Catalog> wrong_type;
      Status s =
          Catalog::Open(storage.get(), pool.get(), heap_root, &wrong_type);
      CHECK("Open on a non-catalog page is rejected",
            !s.ok() && s.code == DbError::kInvalidArgument);
      pool.reset();
      storage->Close();
    }

    // 6b. Bad magic.
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      {
        BufferPool::Handle h;
        pool->Fetch(root, &h);
        std::memset(h.page().data(), 0, 4);
        h.MarkDirty();
        pool->FlushPage(root);
      }
      std::unique_ptr<Catalog> c;
      Status s = Catalog::Open(storage.get(), pool.get(), root, &c);
      CHECK("bad magic rejected", !s.ok());
      pool.reset();
      storage->Close();
    }

    // 6c. Bad version.
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      {
        BufferPool::Handle h;
        pool->Fetch(root, &h);
        std::uint8_t* d = h.page().data();
        d[0] = 0x4Eu; d[1] = 0x43u; d[2] = 0x41u; d[3] = 0x31u;
        RawWriteU16(d + kCatOffVersion, 99);
        h.MarkDirty();
        pool->FlushPage(root);
      }
      std::unique_ptr<Catalog> c;
      CHECK("bad version rejected",
            !Catalog::Open(storage.get(), pool.get(), root, &c).ok());
      pool.reset();
      storage->Close();
    }

    // 6d. Count out of range.
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      {
        BufferPool::Handle h;
        pool->Fetch(root, &h);
        std::uint8_t* d = h.page().data();
        RawWriteU16(d + kCatOffVersion, Catalog::kVersion);
        RawWriteU32(d + kCatOffCount,
                    static_cast<std::uint32_t>(Catalog::kCapacity + 1));
        h.MarkDirty();
        pool->FlushPage(root);
      }
      std::unique_ptr<Catalog> c;
      CHECK("count out of range rejected",
            !Catalog::Open(storage.get(), pool.get(), root, &c).ok());
      pool.reset();
      storage->Close();
    }

    // 6e. Self-cycle in the chain.
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      {
        BufferPool::Handle h;
        pool->Fetch(root, &h);
        std::uint8_t* d = h.page().data();
        RawWriteU32(d + kCatOffCount, 0);
        RawWriteU32(d + kCatOffNext, root);  // self-cycle
        h.MarkDirty();
        pool->FlushPage(root);
      }
      std::unique_ptr<Catalog> c;
      Status s = Catalog::Open(storage.get(), pool.get(), root, &c);
      CHECK("cyclic chain rejected as kBadState",
            !s.ok() && s.code == DbError::kBadState);
      pool.reset();
      storage->Close();
    }

    // 6f. Out-of-range next link.
    {
      std::unique_ptr<PageStorage> storage = OpenStorage(path);
      std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);
      {
        BufferPool::Handle h;
        pool->Fetch(root, &h);
        std::uint8_t* d = h.page().data();
        RawWriteU32(d + kCatOffCount, 0);
        RawWriteU32(d + kCatOffNext, 0xFFFFFFF0u);
        h.MarkDirty();
        pool->FlushPage(root);
      }
      std::unique_ptr<Catalog> c;
      CHECK("out-of-range next link rejected",
            !Catalog::Open(storage.get(), pool.get(), root, &c).ok());
      pool.reset();
      storage->Close();
    }

    RemoveDb(path);
  }

  // ---- 7. Hand-built catalog pages: two-page chain, duplicate, bad refs ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_chain.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 16);

    // Two real heap tables to serve as valid table roots.
    std::unique_ptr<HeapTable> ta, tb;
    Page::PageId root_a = Page::kInvalidId, root_b = Page::kInvalidId;
    HeapTable::Create(storage.get(), pool.get(), &ta, &root_a);
    HeapTable::Create(storage.get(), pool.get(), &tb, &root_b);

    // 7a. Two-page chain C1 -> C2, one table per page.
    Page c1, c2;
    storage->AllocatePage(&c1);
    storage->AllocatePage(&c2);
    BuildCatalogPage(&c1, {{"first", root_a}}, c2.id());
    BuildCatalogPage(&c2, {{"second", root_b}}, Page::kInvalidId);
    storage->WritePage(c1);
    storage->WritePage(c2);

    std::unique_ptr<Catalog> chain;
    CHECK("open a hand-built two-page catalog chain",
          Catalog::Open(storage.get(), pool.get(), c1.id(), &chain).ok());
    CHECK("chain exposes both tables",
          chain != nullptr && chain->table_count() == 2 &&
              chain->Exists("first") && chain->Exists("second"));
    std::unique_ptr<HeapTable> opened;
    CHECK("table from the second chain page opens",
          chain->OpenTable("second", &opened).ok() && opened != nullptr &&
              opened->root() == root_b);

    // 7b. Duplicate names across the chain must be rejected.
    Page d1, d2;
    storage->AllocatePage(&d1);
    storage->AllocatePage(&d2);
    BuildCatalogPage(&d1, {{"dup", root_a}}, d2.id());
    BuildCatalogPage(&d2, {{"dup", root_b}}, Page::kInvalidId);
    storage->WritePage(d1);
    storage->WritePage(d2);
    std::unique_ptr<Catalog> dup;
    Status ds = Catalog::Open(storage.get(), pool.get(), d1.id(), &dup);
    CHECK("duplicate names across pages rejected as kBadState",
          !ds.ok() && ds.code == DbError::kBadState);

    // 7c. Entry referencing a missing page.
    Page e1;
    storage->AllocatePage(&e1);
    BuildCatalogPage(&e1, {{"ghost", 999999u}}, Page::kInvalidId);
    storage->WritePage(e1);
    std::unique_ptr<Catalog> ghost;
    Status gs = Catalog::Open(storage.get(), pool.get(), e1.id(), &ghost);
    CHECK("entry with a missing table page rejected as kBadState",
          !gs.ok() && gs.code == DbError::kBadState);

    // 7d. Entry referencing a page of the wrong type (a data page).
    Page dp;
    storage->AllocatePage(&dp);
    SlottedPage::Init(&dp);
    storage->WritePage(dp);
    Page f1;
    storage->AllocatePage(&f1);
    BuildCatalogPage(&f1, {{"wrongtype", dp.id()}}, Page::kInvalidId);
    storage->WritePage(f1);
    std::unique_ptr<Catalog> wrong;
    Status ws = Catalog::Open(storage.get(), pool.get(), f1.id(), &wrong);
    CHECK("entry pointing at a non-table page rejected as kBadState",
          !ws.ok() && ws.code == DbError::kBadState);

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  // ---- 8. BufferPool pinning failure propagation ----
  {
    const std::string path = TestDbPath("nexusdb_catalog_pinning.db");
    RemoveDb(path);
    std::unique_ptr<PageStorage> storage = OpenStorage(path);
    std::unique_ptr<BufferPool> pool = MakePool(storage.get(), 1);
    std::unique_ptr<Catalog> catalog;
    Page::PageId root = Page::kInvalidId;
    CHECK("pinning: create catalog with a 1-frame pool",
          Catalog::Create(storage.get(), pool.get(), &catalog, &root).ok());

    {
      BufferPool::Handle pin;
      CHECK("pinning: pin the catalog root", pool->Fetch(root, &pin).ok());
      Page::PageId tr = Page::kInvalidId;
      Status s = catalog->CreateTable("blocked", &tr);
      CHECK("CreateTable under a pinned pool is kBufferFull",
            !s.ok() && s.code == DbError::kBufferFull);
      CHECK("failed CreateTable added no table",
            catalog->table_count() == 0 && !catalog->Exists("blocked"));
    }

    // With the pin released the catalog still works.
    Page::PageId tr = Page::kInvalidId;
    CHECK("catalog usable after the failed create",
          catalog->CreateTable("ok_now", &tr).ok() && catalog->Exists("ok_now"));

    pool.reset();
    storage->Close();
    RemoveDb(path);
  }

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
