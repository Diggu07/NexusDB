// NexusDB — Phase 2 buffer-pool test.
//
// Exercises the fixed-capacity BufferPool against a real PageStorage file:
// initialization, cache hits, pin/unpin accounting, dirty tracking, flush,
// persistence across a storage reopen, capacity enforcement, the all-pinned
// error, invalid ids/handles, storage-error propagation, and clean shutdown.
//
// Same plain PASS/FAIL style as the other DB tests: every check goes through
// a real database file in the OS temp directory, removed before and after.
// No internal fields are inspected; only observable behavior is asserted.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace {

using nexusdb::BufferPool;
using nexusdb::DbError;
using nexusdb::Page;
using nexusdb::PageStorage;
using nexusdb::Status;

int g_failures = 0;

#define CHECK(desc, expr)                                     \
  do {                                                        \
    if (expr) {                                               \
      printf("PASS: %s\n", desc);                             \
    } else {                                                  \
      printf("FAIL: %s\n", desc);                             \
      ++g_failures;                                           \
    }                                                         \
  } while (0)

std::string TestDbPath() {
  return (std::filesystem::temp_directory_path() /
          "nexusdb_buffer_pool_test.db")
      .string();
}

void RemoveDb(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Fills a page with a repeating byte pattern and marks it dirty.
void FillPattern(Page* page, unsigned char byte) {
  std::memset(page->data(), byte, Page::size());
  page->MarkDirty();
}

bool MatchesPattern(const Page& page, unsigned char byte) {
  for (std::size_t i = 0; i < Page::size(); ++i) {
    if (page.data()[i] != byte) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  const std::string path = TestDbPath();
  RemoveDb(path);

  // ---- 1. Initialization / argument validation ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for init",
          PageStorage::Open(path, &storage).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("null storage rejected",
          !BufferPool::Create(nullptr, 4, &pool).ok());
    CHECK("pool left null after failed create", pool == nullptr);
    CHECK("zero capacity rejected",
          !BufferPool::Create(storage.get(), 0, &pool).ok());
    CHECK("null out rejected",
          !BufferPool::Create(storage.get(), 4, nullptr).ok());

    CHECK("valid create succeeds",
          BufferPool::Create(storage.get(), 4, &pool).ok());
    CHECK("pool is non-null", pool != nullptr);
    CHECK("capacity reported", pool->capacity() == 4);
    CHECK("pool starts empty", pool->size() == 0);
    CHECK("pool starts with no pins", pool->pinned_frames() == 0);
  }
  RemoveDb(path);

  // ---- 2. Fetch an existing page; cached data matches disk ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for fetch",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate seeded page", storage->AllocatePage(&seeded).ok());
    FillPattern(&seeded, 0x11);
    CHECK("persist seeded page", storage->WritePage(seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create pool", BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle h;
    CHECK("fetch existing page succeeds", pool->Fetch(0, &h).ok());
    CHECK("handle valid", h.valid());
    CHECK("handle reports page id", h.id() == 0);
    CHECK("fetched data matches disk", MatchesPattern(h.page(), 0x11));
    CHECK("fetched page is clean", !h.IsDirty());
    CHECK("page now resident", pool->Contains(0));
    CHECK("size is 1", pool->size() == 1);
  }
  RemoveDb(path);

  // ---- 3. Repeated fetch returns the cached copy (no re-read) ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for repeat",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate repeat page", storage->AllocatePage(&seeded).ok());
    CHECK("persist zeroed repeat page", storage->WritePage(seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create repeat pool", BufferPool::Create(storage.get(), 2, &pool).ok());

    {
      BufferPool::Handle h1;
      CHECK("first fetch", pool->Fetch(0, &h1).ok());
      h1.page().data()[0] = 0xAB;  // change only in memory
      h1.MarkDirty();
    }  // h1 unpinned here

    BufferPool::Handle h2;
    CHECK("second fetch", pool->Fetch(0, &h2).ok());
    CHECK("cached copy returned (not re-read)",
          h2.page().data()[0] == 0xAB);
    CHECK("cached dirty state preserved", h2.IsDirty());
    CHECK("still only one resident page", pool->size() == 1);
  }
  RemoveDb(path);

  // ---- 4. Pin and unpin accounting; move-only handle ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for pins",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate pin page", storage->AllocatePage(&seeded).ok());
    CHECK("persist pin page", storage->WritePage(seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create pin pool", BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle a;
    CHECK("fetch a", pool->Fetch(0, &a).ok());
    CHECK("pin count is 1", pool->PinCount(0) == 1);
    CHECK("one pinned frame", pool->pinned_frames() == 1);

    BufferPool::Handle b;
    CHECK("fetch b", pool->Fetch(0, &b).ok());
    CHECK("pin count is 2", pool->PinCount(0) == 2);

    BufferPool::Handle moved(std::move(b));
    CHECK("move-construct relocates the pin",
          moved.valid() && !b.valid() && pool->PinCount(0) == 2);
    CHECK("b is now empty", !b.valid());

    CHECK("unpin moved", moved.Unpin().ok());
    CHECK("pin count back to 1", pool->PinCount(0) == 1);
    CHECK("one pin still held", pool->pinned_frames() == 1);

    CHECK("unpin a", a.Unpin().ok());
    CHECK("pin count back to 0", pool->PinCount(0) == 0);
    CHECK("no pinned frames left", pool->pinned_frames() == 0);
    CHECK("a is now empty", !a.valid());
  }
  RemoveDb(path);

  // ---- 5. Dirty tracking ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for dirty",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate dirty page", storage->AllocatePage(&seeded).ok());
    CHECK("persist dirty page", storage->WritePage(seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create dirty pool", BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle h;
    CHECK("fetch for dirty", pool->Fetch(0, &h).ok());
    CHECK("freshly fetched page is clean", !h.IsDirty());
    h.MarkDirty();
    CHECK("MarkDirty through handle", h.IsDirty());
    h.page().ClearDirty();
    CHECK("ClearDirty through page", !h.IsDirty());
    h.page().MarkDirty();
    CHECK("MarkDirty through page", h.IsDirty());
  }
  RemoveDb(path);

  // ---- 6. Flush a modified page clears its dirty flag ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for flush",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate flush page", storage->AllocatePage(&seeded).ok());
    CHECK("persist flush page", storage->WritePage(seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create flush pool", BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle h;
    CHECK("fetch for flush", pool->Fetch(0, &h).ok());
    FillPattern(&h.page(), 0x5A);
    CHECK("page dirty after modify", h.IsDirty());
    CHECK("flush resident page", pool->FlushPage(0).ok());
    CHECK("page clean after flush", !h.IsDirty());
    CHECK("flush non-resident page fails",
          !pool->FlushPage(9).ok());
    CHECK("flush non-resident is kNotFound",
          pool->FlushPage(9).code == DbError::kNotFound);
  }
  RemoveDb(path);

  // ---- 7. Persistence across a storage reopen ----
  {
    {
      std::unique_ptr<PageStorage> storage;
      CHECK("open storage for persistence",
            PageStorage::Open(path, &storage).ok());
      Page p1;
      Page p2;
      CHECK("allocate p1", storage->AllocatePage(&p1).ok());
      CHECK("allocate p2", storage->AllocatePage(&p2).ok());

      std::unique_ptr<BufferPool> pool;
      CHECK("create persistence pool",
            BufferPool::Create(storage.get(), 2, &pool).ok());
      {
        BufferPool::Handle h1;
        CHECK("fetch p1", pool->Fetch(0, &h1).ok());
        FillPattern(&h1.page(), 0x7E);
        BufferPool::Handle h2;
        CHECK("fetch p2", pool->Fetch(1, &h2).ok());
        FillPattern(&h2.page(), 0x3C);
        CHECK("flush all dirty", pool->FlushAll().ok());
        CHECK("h1 clean after flush", !h1.IsDirty());
        CHECK("h2 clean after flush", !h2.IsDirty());
      }
    }  // pool destroyed (best-effort flush is a no-op here), then storage closed

    // Reopen storage; a fresh pool must observe the persisted bytes.
    std::unique_ptr<PageStorage> storage;
    CHECK("reopen storage", PageStorage::Open(path, &storage).ok());
    std::unique_ptr<BufferPool> pool;
    CHECK("create reopened pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    BufferPool::Handle h;
    CHECK("fetch p1 after reopen", pool->Fetch(0, &h).ok());
    CHECK("p1 data persisted", MatchesPattern(h.page(), 0x7E));
    CHECK("p1 reads clean", !h.IsDirty());
    BufferPool::Handle h2;
    CHECK("fetch p2 after reopen", pool->Fetch(1, &h2).ok());
    CHECK("p2 data persisted", MatchesPattern(h2.page(), 0x3C));
  }
  RemoveDb(path);

  // ---- 8. Capacity enforcement and deterministic eviction ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for capacity",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("allocate capacity page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create capacity pool", BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("fetch page 0", pool->Fetch(0, &h0).ok());
      BufferPool::Handle h1;
      CHECK("fetch page 1", pool->Fetch(1, &h1).ok());
      CHECK("size at capacity", pool->size() == 2);
    }  // both unpinned

    BufferPool::Handle h2;
    CHECK("fetch page 2 evicts an unpinned frame", pool->Fetch(2, &h2).ok());
    CHECK("size stays at capacity", pool->size() == 2);
    CHECK("new page resident", pool->Contains(2));
    CHECK("lowest unpinned frame evicted (page 0)",
          !pool->Contains(0));
  }
  RemoveDb(path);

  // ---- 9. All frames pinned -> kBufferFull ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for pinned-all",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("allocate pinned-all page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create pinned-all pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    BufferPool::Handle h0;
    BufferPool::Handle h1;
    CHECK("fetch page 0", pool->Fetch(0, &h0).ok());
    CHECK("fetch page 1", pool->Fetch(1, &h1).ok());

    BufferPool::Handle h2;
    const Status s = pool->Fetch(2, &h2);
    CHECK("fetch under all-pinned fails", !s.ok());
    CHECK("all-pinned reports kBufferFull", s.code == DbError::kBufferFull);
    CHECK("failed fetch leaves handle empty", !h2.valid());
    CHECK("pinned pages untouched", pool->Contains(0) && pool->Contains(1));
  }
  RemoveDb(path);

  // ---- 10. Invalid page ids and null outputs ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for invalid ids",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate one page", storage->AllocatePage(&seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create invalid-id pool", BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle h;
    CHECK("null out rejected", !pool->Fetch(0, nullptr).ok());
    CHECK("null out is kInvalidArgument",
          pool->Fetch(0, nullptr).code == DbError::kInvalidArgument);
    CHECK("beyond-EOF id rejected", !pool->Fetch(5, &h).ok());
    CHECK("beyond-EOF is kNotFound",
          pool->Fetch(5, &h).code == DbError::kNotFound);
    CHECK("sentinel id rejected", !pool->Fetch(Page::kInvalidId, &h).ok());
    CHECK("sentinel id is kInvalidArgument",
          pool->Fetch(Page::kInvalidId, &h).code == DbError::kInvalidArgument);
    CHECK("failed fetch leaves handle empty", !h.valid());
    CHECK("nothing cached after failures", pool->size() == 0);
  }
  RemoveDb(path);

  // ---- 11. Invalid unpin operations ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for bad unpin",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate bad-unpin page", storage->AllocatePage(&seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create bad-unpin pool", BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle empty;
    CHECK("unpin empty handle fails", !empty.Unpin().ok());
    CHECK("unpin empty is kInvalidArgument",
          empty.Unpin().code == DbError::kInvalidArgument);

    BufferPool::Handle h;
    CHECK("fetch for unpin", pool->Fetch(0, &h).ok());
    CHECK("first unpin succeeds", h.Unpin().ok());
    CHECK("second unpin fails", !h.Unpin().ok());
    CHECK("unpin count cleared", pool->PinCount(0) == 0);
  }
  RemoveDb(path);

  // ---- 12. Storage error propagation (closed storage) ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for errors",
          PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("allocate error page", storage->AllocatePage(&seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("create error pool", BufferPool::Create(storage.get(), 2, &pool).ok());
    BufferPool::Handle h;
    CHECK("fetch error page", pool->Fetch(0, &h).ok());
    FillPattern(&h.page(), 0x99);
    CHECK("error page dirty", h.IsDirty());
    CHECK("unpin error page", h.Unpin().ok());

    CHECK("close storage under the pool", storage->Close().ok());
    const Status s = pool->FlushAll();
    CHECK("flush over closed storage fails", !s.ok());
    CHECK("closed-storage error is kBadState", s.code == DbError::kBadState);

    // A miss now tries to read through the closed storage.
    BufferPool::Handle missing;
    const Status r = pool->Fetch(1, &missing);
    CHECK("read over closed storage fails", !r.ok());
    CHECK("closed-storage read is kBadState", r.code == DbError::kBadState);
    CHECK("failed read leaves handle empty", !missing.valid());
  }

  RemoveDb(path);
  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
