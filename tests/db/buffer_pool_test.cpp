// NexusDB — Phase 2 buffer-pool test.
//
// Exercises the resizable BufferPool against a real PageStorage file:
// initialization, cache hits, pin/unpin accounting, dirty tracking, flush,
// persistence across a storage reopen, LRU eviction (recency on hits, pinned
// victims skipped, dirty victims written back, failed fetches not disturbing
// state), runtime capacity resizing (grow, shrink, LRU victims, pinned-page
// and dirty-write failures, invalid requests, repeated resizing, and handle
// lifecycle across resizes), the all-pinned error, invalid ids/handles,
// storage-error propagation, and clean shutdown.
//
// Same plain PASS/FAIL style as the other DB tests: every check goes through
// a real database file in the OS temp directory, removed before and after.
// No internal fields are inspected; only observable behavior is asserted.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
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

    // Move assignment must transfer the source's pin and release the pin the
    // target already held, so pins are never leaked or double-released.
    BufferPool::Handle x;
    CHECK("fetch x", pool->Fetch(0, &x).ok());
    BufferPool::Handle y;
    CHECK("fetch y", pool->Fetch(0, &y).ok());
    CHECK("pin count is 2 before move-assign", pool->PinCount(0) == 2);

    y = std::move(x);
    CHECK("move-assign transfers the pin",
          y.valid() && !x.valid() && pool->PinCount(0) == 1);
    CHECK("move-assign released the target's old pin",
          pool->pinned_frames() == 1);
    CHECK("source is now empty after move-assign", !x.valid());

    CHECK("unpin y", y.Unpin().ok());
    CHECK("pin count cleared after move-assign test", pool->PinCount(0) == 0);
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

  // ---- 8. Capacity enforcement and deterministic LRU eviction ----
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
    CHECK("least recently used frame evicted (page 0)",
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

  // ---- 13. LRU: a cache hit promotes the page; the true LRU is evicted ----
  //
  // This sequence is what distinguishes LRU from the old lowest-index policy:
  // after loading 0 then 1 and re-accessing 0, page 0 is most recently used and
  // page 1 is least recently used. A lowest-index policy would evict page 0.
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for LRU recency",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("allocate LRU recency page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create LRU recency pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("LRU recency: fetch 0", pool->Fetch(0, &h0).ok());
      BufferPool::Handle h1;
      CHECK("LRU recency: fetch 1", pool->Fetch(1, &h1).ok());
    }
    {
      BufferPool::Handle h0;
      CHECK("LRU recency: re-fetch 0 promotes it",
            pool->Fetch(0, &h0).ok());
    }

    BufferPool::Handle h2;
    CHECK("LRU recency: fetch 2 succeeds", pool->Fetch(2, &h2).ok());
    CHECK("LRU recency: most recently used page 0 survives",
          pool->Contains(0));
    CHECK("LRU recency: newly loaded page 2 is resident", pool->Contains(2));
    CHECK("LRU recency: least recently used page 1 evicted",
          !pool->Contains(1));
  }
  RemoveDb(path);

  // ---- 14. LRU: a pinned least-recently-used frame is skipped ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for LRU pinned",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("allocate LRU pinned page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create LRU pinned pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle pinned0;
    CHECK("LRU pinned: fetch 0 and pin it", pool->Fetch(0, &pinned0).ok());
    {
      BufferPool::Handle h1;
      CHECK("LRU pinned: fetch 1 then unpin", pool->Fetch(1, &h1).ok());
    }

    BufferPool::Handle h2;
    CHECK("LRU pinned: fetch 2 succeeds", pool->Fetch(2, &h2).ok());
    CHECK("LRU pinned: pinned (least recent) page 0 survives",
          pool->Contains(0));
    CHECK("LRU pinned: page 0 still pinned", pool->PinCount(0) == 1);
    CHECK("LRU pinned: unpinned page 1 evicted", !pool->Contains(1));
    CHECK("LRU pinned: page 2 is resident", pool->Contains(2));
  }
  RemoveDb(path);

  // ---- 15. LRU: all frames pinned -> kBufferFull, no metadata change ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for LRU all-pinned",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("allocate LRU all-pinned page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create LRU all-pinned pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    BufferPool::Handle p0;
    BufferPool::Handle p1;
    CHECK("LRU all-pinned: fetch 0", pool->Fetch(0, &p0).ok());
    CHECK("LRU all-pinned: fetch 1", pool->Fetch(1, &p1).ok());

    BufferPool::Handle h2;
    const Status s = pool->Fetch(2, &h2);
    CHECK("LRU all-pinned: fetch fails", !s.ok());
    CHECK("LRU all-pinned: reports kBufferFull",
          s.code == DbError::kBufferFull);
    CHECK("LRU all-pinned: failed fetch leaves handle empty", !h2.valid());
    CHECK("LRU all-pinned: resident pages untouched",
          pool->Contains(0) && pool->Contains(1));
    CHECK("LRU all-pinned: pin counts untouched",
          pool->PinCount(0) == 1 && pool->PinCount(1) == 1);
  }
  RemoveDb(path);

  // ---- 16. LRU: a dirty victim is written back before its frame is reused ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for LRU dirty victim",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("allocate LRU dirty victim page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create LRU dirty victim pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("LRU dirty victim: fetch 0", pool->Fetch(0, &h0).ok());
      FillPattern(&h0.page(), 0xA5);  // dirty; page 0 becomes least recent
    }
    {
      BufferPool::Handle h1;
      CHECK("LRU dirty victim: fetch 1", pool->Fetch(1, &h1).ok());
    }

    BufferPool::Handle h2;
    CHECK("LRU dirty victim: fetch 2 evicts dirty page 0",
          pool->Fetch(2, &h2).ok());
    CHECK("LRU dirty victim: page 0 evicted", !pool->Contains(0));
    CHECK("LRU dirty victim: page 2 resident", pool->Contains(2));

    // The evicted dirty page must have been written through to the file.
    Page persisted;
    CHECK("LRU dirty victim: evicted page readable from storage",
          storage->ReadPage(0, &persisted).ok());
    CHECK("LRU dirty victim: dirty bytes persisted",
          MatchesPattern(persisted, 0xA5));
    CHECK("LRU dirty victim: persisted page is clean", !persisted.IsDirty());
  }
  RemoveDb(path);

  // ---- 17. LRU: a failed fetch never disturbs residency or recency ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for LRU failed fetch",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 4; ++i) {
      Page p;
      CHECK("allocate LRU failed fetch page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create LRU failed fetch pool",
          BufferPool::Create(storage.get(), 3, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("LRU failed fetch: fetch 0", pool->Fetch(0, &h0).ok());
      BufferPool::Handle h1;
      CHECK("LRU failed fetch: fetch 1", pool->Fetch(1, &h1).ok());
      BufferPool::Handle h2;
      CHECK("LRU failed fetch: fetch 2", pool->Fetch(2, &h2).ok());
    }

    BufferPool::Handle bad;
    const Status s = pool->Fetch(9, &bad);  // beyond EOF: fails before eviction
    CHECK("LRU failed fetch: out-of-range fetch fails", !s.ok());
    CHECK("LRU failed fetch: out-of-range is kNotFound",
          s.code == DbError::kNotFound);
    CHECK("LRU failed fetch: handle left empty", !bad.valid());
    CHECK("LRU failed fetch: residency unchanged by the failure",
          pool->size() == 3 && pool->Contains(0) && pool->Contains(1) &&
              pool->Contains(2));

    // If the failed fetch had advanced recency, page 0 would no longer be the
    // eviction victim here. It must still be page 0.
    BufferPool::Handle h3;
    CHECK("LRU failed fetch: fetch 3 succeeds", pool->Fetch(3, &h3).ok());
    CHECK("LRU failed fetch: recency preserved (page 0 still evicted)",
          !pool->Contains(0));
    CHECK("LRU failed fetch: pages 1-3 resident",
          pool->Contains(1) && pool->Contains(2) && pool->Contains(3));
  }
  RemoveDb(path);

  // ---- 18. LRU: unambiguous victim across repeated evictions ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for LRU sequence",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 5; ++i) {
      Page p;
      CHECK("allocate LRU sequence page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create LRU sequence pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h;
      CHECK("LRU sequence: fetch 0", pool->Fetch(0, &h).ok());
    }
    {
      BufferPool::Handle h;
      CHECK("LRU sequence: fetch 1", pool->Fetch(1, &h).ok());
    }
    {
      BufferPool::Handle h;
      CHECK("LRU sequence: fetch 2 evicts 0", pool->Fetch(2, &h).ok());
    }
    CHECK("LRU sequence: after fetching 2", !pool->Contains(0) &&
                                                pool->Contains(1) &&
                                                pool->Contains(2));
    {
      BufferPool::Handle h;
      CHECK("LRU sequence: fetch 3 evicts 1", pool->Fetch(3, &h).ok());
    }
    CHECK("LRU sequence: after fetching 3", !pool->Contains(1) &&
                                                pool->Contains(2) &&
                                                pool->Contains(3));
    {
      BufferPool::Handle h;
      CHECK("LRU sequence: re-fetch 2 promotes it", pool->Fetch(2, &h).ok());
    }
    BufferPool::Handle h4;
    CHECK("LRU sequence: fetch 4 evicts the promoted-order LRU (3)",
          pool->Fetch(4, &h4).ok());
    CHECK("LRU sequence: page 2 survives, page 3 evicted",
          pool->Contains(2) && pool->Contains(4) && !pool->Contains(3));
    CHECK("LRU sequence: size stays at capacity", pool->size() == 2);
  }
  RemoveDb(path);

  // ---- 19. LRU: state preserved across a failed fetch (write-failure note) ----
  //
  // The dirty-victim WRITE-failure branch of AcquireFrame is intentionally not
  // driven to failure here: PageStorage offers no fault-injection seam, and
  // ReadPage and WritePage share one fstream, so any condition that makes
  // WritePage fail (e.g. a closed file) makes the preceding ReadPage fail
  // first. Fetch would then return at the read step and never reach the flush
  // path -- that closed-storage read failure is covered by section 12. What is
  // checked here is the observable invariant that a failed miss leaves existing
  // resident, dirty state completely intact.
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage for LRU failure state",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 2; ++i) {
      Page p;
      CHECK("allocate LRU failure-state page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create LRU failure-state pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("LRU failure state: fetch 0 and dirty it",
            pool->Fetch(0, &h0).ok());
      FillPattern(&h0.page(), 0x33);  // dirty, unpinned
    }

    CHECK("LRU failure state: close storage", storage->Close().ok());
    BufferPool::Handle missing;
    const Status s = pool->Fetch(1, &missing);  // read fails before eviction
    CHECK("LRU failure state: miss over closed storage fails", !s.ok());
    CHECK("LRU failure state: miss reports kBadState",
          s.code == DbError::kBadState);
    CHECK("LRU failure state: handle left empty", !missing.valid());

    BufferPool::Handle still0;
    CHECK("LRU failure state: resident page still served (cache hit)",
          pool->Fetch(0, &still0).ok());
    CHECK("LRU failure state: dirty flag preserved by the failed miss",
          still0.IsDirty());
    CHECK("LRU failure state: data preserved by the failed miss",
          MatchesPattern(still0.page(), 0x33));
  }
  RemoveDb(path);

  // ---- 20. Resize: increasing capacity preserves residents and pins ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize grow: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("resize grow: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize grow: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle a;
    CHECK("resize grow: fetch 0 and pin", pool->Fetch(0, &a).ok());
    BufferPool::Handle b;
    CHECK("resize grow: fetch 1 and pin", pool->Fetch(1, &b).ok());
    CHECK("resize grow: initial capacity", pool->capacity() == 2);
    CHECK("resize grow: full before growth", pool->size() == 2);

    CHECK("resize grow: increase to 4 succeeds", pool->Resize(4).ok());
    CHECK("resize grow: capacity is 4", pool->capacity() == 4);
    CHECK("resize grow: residents preserved", pool->size() == 2);
    CHECK("resize grow: page 0 still resident", pool->Contains(0));
    CHECK("resize grow: page 1 still resident", pool->Contains(1));
    CHECK("resize grow: pin on page 0 preserved", pool->PinCount(0) == 1);
    CHECK("resize grow: pin on page 1 preserved", pool->PinCount(1) == 1);
    CHECK("resize grow: handle a still valid", a.valid() && a.id() == 0);
    CHECK("resize grow: handle b still valid", b.valid() && b.id() == 1);
    CHECK("resize grow: pins unchanged", pool->pinned_frames() == 2);
  }
  RemoveDb(path);

  // ---- 21. Resize: load more pages after expansion without eviction ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize expand-load: open storage",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 6; ++i) {
      Page p;
      CHECK("resize expand-load: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize expand-load: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("resize expand-load: fetch 0", pool->Fetch(0, &h0).ok());
      BufferPool::Handle h1;
      CHECK("resize expand-load: fetch 1", pool->Fetch(1, &h1).ok());
    }
    CHECK("resize expand-load: increase to 4 succeeds", pool->Resize(4).ok());
    {
      BufferPool::Handle h2;
      CHECK("resize expand-load: fetch 2", pool->Fetch(2, &h2).ok());
      BufferPool::Handle h3;
      CHECK("resize expand-load: fetch 3", pool->Fetch(3, &h3).ok());
    }
    CHECK("resize expand-load: four pages resident", pool->size() == 4);
    CHECK("resize expand-load: pages 0-3 all resident",
          pool->Contains(0) && pool->Contains(1) && pool->Contains(2) &&
              pool->Contains(3));
  }
  RemoveDb(path);

  // ---- 22. Resize: shrinking an underutilized pool keeps its residents ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize under: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 4; ++i) {
      Page p;
      CHECK("resize under: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize under: create pool",
          BufferPool::Create(storage.get(), 4, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("resize under: fetch 0", pool->Fetch(0, &h0).ok());
    }
    CHECK("resize under: one resident page", pool->size() == 1);

    CHECK("resize under: shrink to 2 succeeds", pool->Resize(2).ok());
    CHECK("resize under: capacity is 2", pool->capacity() == 2);
    CHECK("resize under: resident page intact", pool->Contains(0));
    CHECK("resize under: size intact", pool->size() == 1);
  }
  RemoveDb(path);

  // ---- 23. Resize: shrinking a full pool evicts the LRU pages ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize lru: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 4; ++i) {
      Page p;
      CHECK("resize lru: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize lru: create pool",
          BufferPool::Create(storage.get(), 4, &pool).ok());
    {
      BufferPool::Handle h;
      CHECK("resize lru: fetch 0", pool->Fetch(0, &h).ok());
    }
    {
      BufferPool::Handle h;
      CHECK("resize lru: fetch 1", pool->Fetch(1, &h).ok());
    }
    {
      BufferPool::Handle h;
      CHECK("resize lru: fetch 2", pool->Fetch(2, &h).ok());
    }
    {
      BufferPool::Handle h;
      CHECK("resize lru: fetch 3", pool->Fetch(3, &h).ok());
    }
    {
      BufferPool::Handle h;
      CHECK("resize lru: re-fetch 0 to promote it", pool->Fetch(0, &h).ok());
    }
    // Recency (oldest first): 1, 2, 3, 0. Shrinking to 2 must evict 1 and 2.
    CHECK("resize lru: shrink to 2 succeeds", pool->Resize(2).ok());
    CHECK("resize lru: capacity is 2", pool->capacity() == 2);
    CHECK("resize lru: size is 2", pool->size() == 2);
    CHECK("resize lru: most recently used pages survive",
          pool->Contains(0) && pool->Contains(3));
    CHECK("resize lru: least recently used pages evicted",
          !pool->Contains(1) && !pool->Contains(2));
  }
  RemoveDb(path);

  // ---- 24. Resize: shrink fails safely when too many pages are pinned ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize pinned: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("resize pinned: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize pinned: create pool",
          BufferPool::Create(storage.get(), 3, &pool).ok());
    BufferPool::Handle p0;
    CHECK("resize pinned: fetch 0 and pin", pool->Fetch(0, &p0).ok());
    BufferPool::Handle p1;
    CHECK("resize pinned: fetch 1 and pin", pool->Fetch(1, &p1).ok());
    {
      BufferPool::Handle h2;
      CHECK("resize pinned: fetch 2 (left unpinned)", pool->Fetch(2, &h2).ok());
    }

    const Status s = pool->Resize(1);  // needs 2 victims, but only page 2 can go
    CHECK("resize pinned: shrink fails", !s.ok());
    CHECK("resize pinned: reports kBufferFull", s.code == DbError::kBufferFull);
    CHECK("resize pinned: capacity unchanged", pool->capacity() == 3);
    CHECK("resize pinned: all pages still resident",
          pool->Contains(0) && pool->Contains(1) && pool->Contains(2));
    CHECK("resize pinned: size unchanged", pool->size() == 3);
    CHECK("resize pinned: pins on 0 and 1 unchanged",
          pool->PinCount(0) == 1 && pool->PinCount(1) == 1);
    CHECK("resize pinned: handles still valid",
          p0.valid() && p0.id() == 0 && p1.valid() && p1.id() == 1);
  }
  RemoveDb(path);

  // ---- 25. Resize: dirty LRU victims are written back before eviction ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize dirty: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("resize dirty: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize dirty: create pool",
          BufferPool::Create(storage.get(), 3, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("resize dirty: fetch 0", pool->Fetch(0, &h0).ok());
      FillPattern(&h0.page(), 0x11);
    }
    {
      BufferPool::Handle h1;
      CHECK("resize dirty: fetch 1", pool->Fetch(1, &h1).ok());
      FillPattern(&h1.page(), 0x22);
    }
    {
      BufferPool::Handle h2;
      CHECK("resize dirty: fetch 2", pool->Fetch(2, &h2).ok());
    }

    // Recency (oldest first): 0, 1, 2. Shrinking to 1 evicts dirty 0 and 1.
    CHECK("resize dirty: shrink to 1 succeeds", pool->Resize(1).ok());
    CHECK("resize dirty: capacity is 1", pool->capacity() == 1);
    CHECK("resize dirty: only most recent page resident",
          pool->Contains(2) && !pool->Contains(0) && !pool->Contains(1));

    Page p0;
    CHECK("resize dirty: evicted page 0 readable",
          storage->ReadPage(0, &p0).ok());
    CHECK("resize dirty: page 0 bytes persisted", MatchesPattern(p0, 0x11));
    Page p1;
    CHECK("resize dirty: evicted page 1 readable",
          storage->ReadPage(1, &p1).ok());
    CHECK("resize dirty: page 1 bytes persisted", MatchesPattern(p1, 0x22));
  }
  RemoveDb(path);

  // ---- 26. Resize: a dirty-victim write failure aborts the shrink ----
  //
  // PageStorage exposes no fault-injection seam, but closing it makes
  // WritePage fail deterministically while the pool's metadata stays readable
  // (the same closed-storage technique section 12 uses). A shrink that must
  // write a dirty victim therefore fails without changing capacity, residency,
  // or the victim's dirty state.
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize write-fail: open storage",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 2; ++i) {
      Page p;
      CHECK("resize write-fail: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize write-fail: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("resize write-fail: fetch 0", pool->Fetch(0, &h0).ok());
      FillPattern(&h0.page(), 0x33);  // dirty, and least recently used
    }
    {
      BufferPool::Handle h1;
      CHECK("resize write-fail: fetch 1", pool->Fetch(1, &h1).ok());
    }

    CHECK("resize write-fail: close storage", storage->Close().ok());
    const Status s = pool->Resize(1);  // victim page 0 is dirty -> write fails
    CHECK("resize write-fail: shrink fails", !s.ok());
    CHECK("resize write-fail: reports kBadState", s.code == DbError::kBadState);
    CHECK("resize write-fail: capacity unchanged", pool->capacity() == 2);
    CHECK("resize write-fail: residents unchanged",
          pool->size() == 2 && pool->Contains(0) && pool->Contains(1));

    BufferPool::Handle still0;
    CHECK("resize write-fail: victim still resident (cache hit)",
          pool->Fetch(0, &still0).ok());
    CHECK("resize write-fail: victim still dirty", still0.IsDirty());
    CHECK("resize write-fail: victim data intact",
          MatchesPattern(still0.page(), 0x33));
  }
  RemoveDb(path);

  // A shrink whose victims are all clean needs no I/O, so it still succeeds
  // even after the storage has been closed.
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize clean-offline: open storage",
          PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 2; ++i) {
      Page p;
      CHECK("resize clean-offline: allocate page",
            storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize clean-offline: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("resize clean-offline: fetch 0", pool->Fetch(0, &h0).ok());
    }
    {
      BufferPool::Handle h1;
      CHECK("resize clean-offline: fetch 1", pool->Fetch(1, &h1).ok());
    }

    CHECK("resize clean-offline: close storage", storage->Close().ok());
    CHECK("resize clean-offline: clean shrink succeeds", pool->Resize(1).ok());
    CHECK("resize clean-offline: capacity is 1", pool->capacity() == 1);
    CHECK("resize clean-offline: LRU clean page evicted", !pool->Contains(0));
    CHECK("resize clean-offline: most recent page retained",
          pool->Contains(1));
  }
  RemoveDb(path);

  // ---- 27. Resize: invalid requests leave the pool unchanged ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize invalid: open storage", PageStorage::Open(path, &storage).ok());
    Page seeded;
    CHECK("resize invalid: allocate page", storage->AllocatePage(&seeded).ok());

    std::unique_ptr<BufferPool> pool;
    CHECK("resize invalid: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("resize invalid: fetch 0", pool->Fetch(0, &h0).ok());
    }

    const Status z = pool->Resize(0);
    CHECK("resize invalid: zero capacity rejected", !z.ok());
    CHECK("resize invalid: zero is kInvalidArgument",
          z.code == DbError::kInvalidArgument);
    CHECK("resize invalid: zero left pool unchanged",
          pool->capacity() == 2 && pool->Contains(0) && pool->size() == 1);

    const Status huge = pool->Resize(std::numeric_limits<std::size_t>::max());
    CHECK("resize invalid: absurd capacity rejected", !huge.ok());
    CHECK("resize invalid: absurd is kOutOfMemory",
          huge.code == DbError::kOutOfMemory);
    CHECK("resize invalid: absurd left pool unchanged",
          pool->capacity() == 2 && pool->Contains(0) && pool->size() == 1);
  }
  RemoveDb(path);

  // ---- 28. Resize: repeated grow/shrink stays consistent ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize repeat: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 6; ++i) {
      Page p;
      CHECK("resize repeat: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize repeat: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());

    CHECK("resize repeat: grow to 4",
          pool->Resize(4).ok() && pool->capacity() == 4);
    for (int i = 0; i < 4; ++i) {
      BufferPool::Handle h;
      CHECK("resize repeat: load page",
            pool->Fetch(static_cast<Page::PageId>(i), &h).ok());
    }
    CHECK("resize repeat: four resident", pool->size() == 4);

    CHECK("resize repeat: grow to 6",
          pool->Resize(6).ok() && pool->capacity() == 6);
    {
      BufferPool::Handle h4;
      CHECK("resize repeat: load page 4", pool->Fetch(4, &h4).ok());
      BufferPool::Handle h5;
      CHECK("resize repeat: load page 5", pool->Fetch(5, &h5).ok());
    }
    CHECK("resize repeat: six resident", pool->size() == 6);

    // Recency (oldest first): 0,1,2,3,4,5. Shrinking to 3 evicts 0,1,2.
    CHECK("resize repeat: shrink to 3",
          pool->Resize(3).ok() && pool->capacity() == 3);
    CHECK("resize repeat: size clamped to 3", pool->size() == 3);
    CHECK("resize repeat: most recent three survive",
          pool->Contains(3) && pool->Contains(4) && pool->Contains(5));
    CHECK("resize repeat: least recent three evicted",
          !pool->Contains(0) && !pool->Contains(1) && !pool->Contains(2));

    CHECK("resize repeat: grow again to 5",
          pool->Resize(5).ok() && pool->capacity() == 5);
    {
      BufferPool::Handle h0;
      CHECK("resize repeat: refetch 0 fits without eviction",
            pool->Fetch(0, &h0).ok());
    }
    CHECK("resize repeat: four resident after refetch", pool->size() == 4);
    CHECK("resize repeat: all four present",
          pool->Contains(0) && pool->Contains(3) && pool->Contains(4) &&
              pool->Contains(5));
    CHECK("resize repeat: capacity still 5", pool->capacity() == 5);
  }
  RemoveDb(path);

  // ---- 29. Resize: same-capacity resize is a no-op ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize noop: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 2; ++i) {
      Page p;
      CHECK("resize noop: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize noop: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());
    {
      BufferPool::Handle h0;
      CHECK("resize noop: fetch 0", pool->Fetch(0, &h0).ok());
      FillPattern(&h0.page(), 0x44);  // dirty
    }
    {
      BufferPool::Handle h1;
      CHECK("resize noop: fetch 1", pool->Fetch(1, &h1).ok());
    }

    CHECK("resize noop: resize to same capacity succeeds", pool->Resize(2).ok());
    CHECK("resize noop: capacity unchanged", pool->capacity() == 2);
    CHECK("resize noop: residents unchanged",
          pool->size() == 2 && pool->Contains(0) && pool->Contains(1));

    BufferPool::Handle h0;
    CHECK("resize noop: no flush happened (page 0 still dirty)",
          pool->Fetch(0, &h0).ok() && h0.IsDirty());
  }
  RemoveDb(path);

  // ---- 30. Resize: handle lifecycle stays valid across grow and shrink ----
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("resize handle: open storage", PageStorage::Open(path, &storage).ok());
    for (int i = 0; i < 3; ++i) {
      Page p;
      CHECK("resize handle: allocate page", storage->AllocatePage(&p).ok());
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("resize handle: create pool",
          BufferPool::Create(storage.get(), 2, &pool).ok());

    BufferPool::Handle a;
    CHECK("resize handle: fetch 0", pool->Fetch(0, &a).ok());
    BufferPool::Handle b(std::move(a));
    CHECK("resize handle: move-construct relocates the pin",
          b.valid() && !a.valid() && pool->PinCount(0) == 1);

    CHECK("resize handle: grow to 4 succeeds", pool->Resize(4).ok());
    CHECK("resize handle: handle valid after grow", b.valid() && b.id() == 0);
    CHECK("resize handle: pin intact after grow", pool->PinCount(0) == 1);

    BufferPool::Handle c;
    CHECK("resize handle: fetch 1 into c", pool->Fetch(1, &c).ok());
    CHECK("resize handle: two pins held",
          pool->PinCount(0) == 1 && pool->PinCount(1) == 1);
    c = std::move(b);
    CHECK("resize handle: move-assign transfers the pin",
          c.valid() && !b.valid() && c.id() == 0);
    CHECK("resize handle: move-assign released the target's old pin",
          pool->PinCount(1) == 0 && pool->PinCount(0) == 1);

    // Page 0 is pinned by c, so a shrink to 1 evicts the unpinned page 1.
    CHECK("resize handle: shrink to 1 succeeds", pool->Resize(1).ok());
    CHECK("resize handle: pinned page retained", pool->Contains(0));
    CHECK("resize handle: unpinned page evicted", !pool->Contains(1));
    CHECK("resize handle: handle c still valid after shrink",
          c.valid() && c.id() == 0 && pool->PinCount(0) == 1);

    CHECK("resize handle: unpin c", c.Unpin().ok());
    CHECK("resize handle: pin count cleared", pool->PinCount(0) == 0);
  }
  RemoveDb(path);

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
