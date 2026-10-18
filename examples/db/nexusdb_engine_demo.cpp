// NexusDB — comprehensive Database Engine demonstration
// (Digvijay: Database Engine, Phases 1 and 2).
//
// A single, self-contained, runnable program that exercises EVERY database
// engine feature currently implemented in this repository, using only the
// real public API of Page, PageStorage, and BufferPool. Nothing here is
// simulated: every PASS comes from an actual assertion on observed state or a
// real Status return.
//
// Sections:
//   [1] Page storage and persistence
//   [2] Buffer-pool caching
//   [3] LRU eviction
//   [4] Dynamic buffer-pool resizing (growth, shrink, pinned safety, invalid)
//   [5] Pinning and handle lifecycle
//   [6] Dirty-page write-back (flush and eviction)
//   [7] Error handling
//   [8] Final verification summary
//
// It works on a uniquely named temporary database file, never touches any
// other database file, releases every handle before destroying any pool, and
// closes storage before deleting the file. It exits with code 0 only when
// every check passes.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace {

using nexusdb::BufferPool;
using nexusdb::DbError;
using nexusdb::DbErrorString;
using nexusdb::Page;
using nexusdb::PageStorage;
using nexusdb::Status;

int g_checks = 0;
int g_passed = 0;
int g_failed = 0;

// Records and prints one real check. FAIL is printed only when the actual
// assertion is false.
void Check(const std::string& description, bool condition) {
  ++g_checks;
  if (condition) {
    ++g_passed;
    std::printf("    [PASS] %s\n", description.c_str());
  } else {
    ++g_failed;
    std::printf("    [FAIL] %s\n", description.c_str());
  }
}

// Informational line (not a check): observations, API notes, limitations.
void Info(const std::string& text) {
  std::printf("    %s\n", text.c_str());
}

void Section(const char* heading) {
  std::printf("\n%s\n", heading);
}

// Page id -> recognizable byte pattern used throughout the demo.
std::uint8_t PatternFor(Page::PageId id) {
  return static_cast<std::uint8_t>(0x40 + id);
}

void FillPattern(Page* page, std::uint8_t value) {
  std::memset(page->data(), value, Page::size());
}

bool MatchesPattern(const Page& page, std::uint8_t value) {
  for (std::size_t i = 0; i < Page::size(); ++i) {
    if (page.data()[i] != value) {
      return false;
    }
  }
  return true;
}

// A unique, dedicated temporary database path. The timestamp suffix keeps it
// from ever colliding with an existing file, so the demo cannot overwrite or
// delete a user's database.
std::string DemoDbPath() {
  namespace fs = std::filesystem;
  const auto stamp = static_cast<long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string name =
      "nexusdb_engine_demo_" + std::to_string(stamp) + ".db";
  return (fs::temp_directory_path() / name).string();
}

}  // namespace

int main() {
  const std::string path = DemoDbPath();

  std::printf(
      "================================================================\n"
      "                NEXUSDB DATABASE ENGINE DEMO\n"
      "                   One Nexus, Zero Waste\n"
      "================================================================\n");
  std::printf("Temporary database: %s\n", path.c_str());
  std::printf("Page size: %u bytes\n", nexusdb::kPageSize);

  constexpr Page::PageId kPageCount = 12;

  // ==================================================================
  Section("[1] PAGE STORAGE AND PERSISTENCE");
  // ==================================================================
  Info("PageStorage maps fixed-size pages onto one database file and is the");
  Info("only layer that touches it. This is raw page I/O, not SQL or CRUD.");

  std::unique_ptr<PageStorage> storage;
  {
    std::unique_ptr<PageStorage> initial;
    Status s = PageStorage::Open(path, &initial);
    Check("PageStorage::Open created the database file",
          s.ok() && initial != nullptr);
    if (!initial) {
      std::printf("FATAL: cannot continue without a database file\n");
      std::error_code ec;
      std::filesystem::remove(path, ec);
      return 1;
    }

    std::uint32_t count = 0;
    Check("a fresh database reports 0 pages",
          initial->NumPages(&count).ok() && count == 0);

    bool allocation_ok = true;
    bool writes_ok = true;
    for (Page::PageId i = 0; i < kPageCount; ++i) {
      Page page;
      Status a = initial->AllocatePage(&page);
      if (!a.ok() || page.id() != i) {
        allocation_ok = false;
      }
      FillPattern(&page, PatternFor(i));
      if (!initial->WritePage(page).ok()) {
        writes_ok = false;
      }
    }
    Check("allocated pages 0.." + std::to_string(kPageCount - 1) +
              " with dense, sequential ids",
          allocation_ok);
    Check("wrote a known byte pattern into every allocated page", writes_ok);
    Check("page count is now " + std::to_string(kPageCount),
          initial->NumPages(&count).ok() && count == kPageCount);
    Check("closed the database cleanly", initial->Close().ok());
  }  // initial destroyed

  // Reopen the SAME file to prove the bytes were persisted.
  Status reopen = PageStorage::Open(path, &storage);
  Check("reopened the same database file", reopen.ok() && storage != nullptr);
  if (!storage) {
    std::printf("FATAL: cannot reopen the database file\n");
    return 1;
  }

  std::uint32_t count_after = 0;
  Check("page count survived reopen",
        storage->NumPages(&count_after).ok() && count_after == kPageCount);

  std::uint32_t matched = 0;
  bool ids_ok = true;
  bool clean_ok = true;
  for (Page::PageId i = 0; i < kPageCount; ++i) {
    Page page;
    Status r = storage->ReadPage(i, &page);
    const bool ok = r.ok() && page.id() == i && MatchesPattern(page, PatternFor(i));
    if (ok) {
      ++matched;
    }
    if (!(r.ok() && page.id() == i)) {
      ids_ok = false;
    }
    if (!(r.ok() && !page.IsDirty())) {
      clean_ok = false;
    }
  }
  Check("all " + std::to_string(kPageCount) +
            " pages match their written patterns after reopen",
        matched == kPageCount);
  Check("all page ids are correct after reopen", ids_ok);
  Check("pages read back from storage are clean", clean_ok);

  // ==================================================================
  Section("[2] BUFFER-POOL CACHING");
  // ==================================================================
  Info("BufferPool caches pages in frames on top of PageStorage, so a page");
  Info("stays in memory instead of being re-read from disk.");

  std::unique_ptr<BufferPool> cache_pool;
  Check("created a buffer pool with capacity 3",
        BufferPool::Create(storage.get(), 3, &cache_pool).ok() &&
            cache_pool != nullptr);
  {
    BufferPool::Handle h;
    Check("fetch page 3 into the pool", cache_pool->Fetch(3, &h).ok());
    Check("page 3 is now resident", cache_pool->Contains(3));
    Check("page 3 has pin count 1", cache_pool->PinCount(3) == 1);
    Check("the pool holds exactly 1 page", cache_pool->size() == 1);
    Check("cached page data matches what is on disk",
          h.valid() && MatchesPattern(h.page(), PatternFor(3)));
  }
  {
    BufferPool::Handle h1;
    Check("fetch page 3 again (served from memory)",
          cache_pool->Fetch(3, &h1).ok());
    Check("page 3 is not duplicated: still 1 resident page",
          cache_pool->Contains(3) && cache_pool->size() == 1);
    BufferPool::Handle h2;
    Check("fetch page 3 once more into a second live handle",
          cache_pool->Fetch(3, &h2).ok());
    Check("two live handles pin page 3 (pin count 2)",
          cache_pool->PinCount(3) == 2);
  }
  Check("after releasing both handles page 3 is unpinned but still resident",
        cache_pool->PinCount(3) == 0 && cache_pool->Contains(3));
  Info("Note: the public API exposes no cache-hit counter, so a hit is shown");
  Info("by residency, a constant size, and pin counts -- not by a statistic.");
  cache_pool.reset();

  // ==================================================================
  Section("[3] LRU EVICTION (capacity 2)");
  // ==================================================================
  Info("Replacement is true LRU: the least recently used unpinned page is");
  Info("evicted when a non-resident page is needed and the pool is full.");

  std::unique_ptr<BufferPool> lru_pool;
  Check("created a buffer pool with capacity 2",
        BufferPool::Create(storage.get(), 2, &lru_pool).ok() &&
            lru_pool != nullptr);
  {
    BufferPool::Handle a;
    Check("fetch A (page 0)", lru_pool->Fetch(0, &a).ok());
  }
  {
    BufferPool::Handle b;
    Check("fetch B (page 1)", lru_pool->Fetch(1, &b).ok());
  }
  {
    BufferPool::Handle a;
    Check("fetch A (page 0) again -> A becomes most recently used",
          lru_pool->Fetch(0, &a).ok());
  }
  {
    BufferPool::Handle c;
    Check("fetch C (page 2) -> pool is full, must evict the LRU page",
          lru_pool->Fetch(2, &c).ok());
    Check("A (page 0) remains resident", lru_pool->Contains(0));
    Check("B (page 1) was evicted", !lru_pool->Contains(1));
    Check("C (page 2) is resident", lru_pool->Contains(2));
    Check("resident count stays within capacity (== 2)",
          lru_pool->size() == 2 && lru_pool->size() <= lru_pool->capacity());
  }
  Info("Access order A -> B -> A -> C leaves A and C resident; B, the least");
  Info("recently used page, is evicted.");
  lru_pool.reset();

  // ==================================================================
  Section("[4] DYNAMIC BUFFER-POOL RESIZING");
  // ==================================================================

  // ---- A. Growth ---------------------------------------------------
  std::printf("  -- A. Growth (2 -> 4)\n");
  std::unique_ptr<BufferPool> grow_pool;
  Check("created a buffer pool with capacity 2",
        BufferPool::Create(storage.get(), 2, &grow_pool).ok() &&
            grow_pool != nullptr);
  {
    BufferPool::Handle ga;
    BufferPool::Handle gb;
    Check("fetch page 0 and keep its handle (pinned)",
          grow_pool->Fetch(0, &ga).ok());
    Check("fetch page 1 and keep its handle (pinned)",
          grow_pool->Fetch(1, &gb).ok());
    Check("two pages resident, both pinned",
          grow_pool->size() == 2 && grow_pool->pinned_frames() == 2);
    Check("grow capacity 2 -> 4", grow_pool->Resize(4).ok());
    Check("capacity is now 4", grow_pool->capacity() == 4);
    Check("resident pages preserved across growth",
          grow_pool->Contains(0) && grow_pool->Contains(1));
    Check("active handles remain valid across growth",
          ga.valid() && gb.valid());
    Check("pin counts preserved across growth",
          grow_pool->PinCount(0) == 1 && grow_pool->PinCount(1) == 1);
    {
      BufferPool::Handle h2;
      BufferPool::Handle h3;
      Check("fetch two more pages (2 and 3)",
            grow_pool->Fetch(2, &h2).ok() && grow_pool->Fetch(3, &h3).ok());
      Check("four pages now resident, none evicted",
            grow_pool->size() == 4 && grow_pool->Contains(0) &&
                grow_pool->Contains(1) && grow_pool->Contains(2) &&
                grow_pool->Contains(3));
    }
    ga.Unpin();
    gb.Unpin();
  }
  grow_pool.reset();

  // ---- B. Shrink ---------------------------------------------------
  std::printf("  -- B. Shrink (4 -> 2) with LRU survivors\n");
  std::unique_ptr<BufferPool> shrink_pool;
  Check("created a buffer pool with capacity 4",
        BufferPool::Create(storage.get(), 4, &shrink_pool).ok() &&
            shrink_pool != nullptr);
  {
    BufferPool::Handle h;
    Check("load page 0", shrink_pool->Fetch(0, &h).ok());
  }
  {
    BufferPool::Handle h;
    Check("load page 1", shrink_pool->Fetch(1, &h).ok());
  }
  {
    BufferPool::Handle h;
    Check("load page 2", shrink_pool->Fetch(2, &h).ok());
  }
  {
    BufferPool::Handle h;
    Check("load page 3", shrink_pool->Fetch(3, &h).ok());
  }
  {
    BufferPool::Handle h;
    Check("touch page 2 (promote its recency)", shrink_pool->Fetch(2, &h).ok());
  }
  {
    BufferPool::Handle h;
    Check("touch page 3 (promote its recency)", shrink_pool->Fetch(3, &h).ok());
  }
  Check("four pages resident before shrink", shrink_pool->size() == 4);
  Check("shrink capacity 4 -> 2", shrink_pool->Resize(2).ok());
  Check("capacity is now 2", shrink_pool->capacity() == 2);
  Check("resident count reduced to 2", shrink_pool->size() == 2);
  Check("most recently used pages 2 and 3 survived",
        shrink_pool->Contains(2) && shrink_pool->Contains(3));
  Check("least recently used pages 0 and 1 were evicted",
        !shrink_pool->Contains(0) && !shrink_pool->Contains(1));
  shrink_pool.reset();

  // ---- C. Pinned-page safety --------------------------------------
  std::printf("  -- C. Pinned-page safety\n");
  std::unique_ptr<BufferPool> pin_pool;
  Check("created a buffer pool with capacity 3",
        BufferPool::Create(storage.get(), 3, &pin_pool).ok() &&
            pin_pool != nullptr);
  {
    BufferPool::Handle p4;
    BufferPool::Handle p5;
    BufferPool::Handle p6;
    Check("pin pages 4, 5 and 6",
          pin_pool->Fetch(4, &p4).ok() && pin_pool->Fetch(5, &p5).ok() &&
              pin_pool->Fetch(6, &p6).ok());
    Check("all three frames are pinned", pin_pool->pinned_frames() == 3);
    Status st = pin_pool->Resize(1);
    Check("shrink to 1 is rejected", !st.ok());
    Check("error category is kBufferFull (" +
              std::string(DbErrorString(st.code)) + ")",
          st.code == DbError::kBufferFull);
    Check("capacity unchanged (3)", pin_pool->capacity() == 3);
    Check("residency unchanged",
          pin_pool->size() == 3 && pin_pool->Contains(4) &&
              pin_pool->Contains(5) && pin_pool->Contains(6));
    Check("pin counts unchanged",
          pin_pool->PinCount(4) == 1 && pin_pool->PinCount(5) == 1 &&
              pin_pool->PinCount(6) == 1);
    Check("handles remain valid", p4.valid() && p5.valid() && p6.valid());
    p4.Unpin();
    p5.Unpin();
    p6.Unpin();
  }
  pin_pool.reset();

  // ---- D. Invalid capacity ----------------------------------------
  std::printf("  -- D. Invalid capacity\n");
  std::unique_ptr<BufferPool> cap_pool;
  Check("created a buffer pool with capacity 3",
        BufferPool::Create(storage.get(), 3, &cap_pool).ok() &&
            cap_pool != nullptr);
  {
    BufferPool::Handle h;
    Check("load page 0", cap_pool->Fetch(0, &h).ok());
  }
  Status zero = cap_pool->Resize(0);
  Check("Resize(0) is rejected with kInvalidArgument",
        !zero.ok() && zero.code == DbError::kInvalidArgument);
  Check("capacity unchanged after rejected resize",
        cap_pool->capacity() == 3);
  Check("residency unchanged after rejected resize",
        cap_pool->size() == 1 && cap_pool->Contains(0));
  Status huge = cap_pool->Resize(std::numeric_limits<std::size_t>::max());
  Check("an absurd capacity is rejected with kOutOfMemory",
        !huge.ok() && huge.code == DbError::kOutOfMemory);
  Check("capacity still unchanged after absurd resize",
        cap_pool->capacity() == 3);
  cap_pool.reset();

  // ==================================================================
  Section("[5] PINNING AND HANDLE LIFECYCLE");
  // ==================================================================
  Info("A Handle pins its page while alive, is move-only, and unpins on");
  Info("destruction or Unpin(). Pinned pages are never evicted.");

  std::unique_ptr<BufferPool> life_pool;
  Check("created a buffer pool with capacity 2",
        BufferPool::Create(storage.get(), 2, &life_pool).ok() &&
            life_pool != nullptr);
  {
    BufferPool::Handle a;
    Check("fetch page 0 -> pin count 1",
          life_pool->Fetch(0, &a).ok() && life_pool->PinCount(0) == 1);
    BufferPool::Handle b;
    Check("fetch page 0 again -> pin count 2",
          life_pool->Fetch(0, &b).ok() && life_pool->PinCount(0) == 2);
    Check("Unpin() releases one pin -> pin count 1",
          b.Unpin().ok() && life_pool->PinCount(0) == 1);
    Check("the unpinned handle is now empty", !b.valid());

    BufferPool::Handle c(std::move(a));
    Check("move-construction empties the source handle", !a.valid());
    Check("move-construction keeps the pin (pin count 1)",
          c.valid() && life_pool->PinCount(0) == 1);

    BufferPool::Handle d;
    Check("fetch page 1 into handle d (pin count 1)",
          life_pool->Fetch(1, &d).ok() && life_pool->PinCount(1) == 1);
    d = std::move(c);
    Check("move-assignment transfers the pin to page 0",
          life_pool->PinCount(0) == 1 && d.valid() && d.id() == 0);
    Check("move-assignment released d's previous pin on page 1",
          life_pool->PinCount(1) == 0);
    Check("source handle is empty after move-assignment", !c.valid());

    BufferPool::Handle e;
    Check("fetch page 1 (pin it)", life_pool->Fetch(1, &e).ok());
    Check("both frames are now pinned", life_pool->pinned_frames() == 2);
    BufferPool::Handle f;
    Status fs = life_pool->Fetch(2, &f);
    Check("fetching a third page fails while every frame is pinned",
          !fs.ok() && fs.code == DbError::kBufferFull);
    Check("the failed fetch leaves its handle empty", !f.valid());
    Check("pinned pages remain resident",
          life_pool->Contains(0) && life_pool->Contains(1));
    d.Unpin();
    e.Unpin();

    BufferPool::Handle g;
    Check("fetch page 1 (pin it)", life_pool->Fetch(1, &g).ok());
    Check("grow to 4 while holding a live handle", life_pool->Resize(4).ok());
    Check("handle valid after growth, pin intact",
          g.valid() && life_pool->PinCount(1) == 1);
    Check("shrink to 2 keeps the pinned page",
          life_pool->Resize(2).ok() && life_pool->Contains(1));
    Check("handle still valid after shrink", g.valid());
    g.Unpin();
  }
  Check("every handle released -> no pinned frames",
        life_pool->pinned_frames() == 0);
  life_pool.reset();

  // ==================================================================
  Section("[6] DIRTY-PAGE WRITE-BACK");
  // ==================================================================
  Info("A dirty page is written to storage by FlushPage/FlushAll, and a dirty");
  Info("victim is written back before its frame is reused.");

  std::unique_ptr<BufferPool> dirty_pool;
  Check("created a buffer pool with capacity 2",
        BufferPool::Create(storage.get(), 2, &dirty_pool).ok() &&
            dirty_pool != nullptr);
  {
    BufferPool::Handle h;
    Check("fetch page 5", dirty_pool->Fetch(5, &h).ok());
    Check("a freshly fetched page is clean", !h.IsDirty());
    FillPattern(&h.page(), 0xE1);
    h.MarkDirty();
    Check("page is dirty after modification", h.IsDirty());
    Check("FlushPage writes it back", dirty_pool->FlushPage(5).ok());
    Check("page is clean after flush", !h.IsDirty());
    Page disk;
    Check("modified bytes reached storage",
          storage->ReadPage(5, &disk).ok() && MatchesPattern(disk, 0xE1));
  }
  {
    BufferPool::Handle h;
    Check("fetch page 6", dirty_pool->Fetch(6, &h).ok());
    FillPattern(&h.page(), 0xE2);
    h.MarkDirty();
    Check("page 6 is dirty", h.IsDirty());
  }
  {
    BufferPool::Handle h;
    Check("fetch page 7 (fills the pool)", dirty_pool->Fetch(7, &h).ok());
    FillPattern(&h.page(), 0xE3);
    h.MarkDirty();
  }
  {
    BufferPool::Handle h;
    Check("fetch page 8 -> evicts the LRU dirty page 6",
          dirty_pool->Fetch(8, &h).ok());
    Check("page 6 was evicted", !dirty_pool->Contains(6));
    Page disk;
    Check("evicted dirty bytes were written back",
          storage->ReadPage(6, &disk).ok() && MatchesPattern(disk, 0xE2));
  }
  Info("Note: this demo exercises the buffer-pool layer only (normal");
  Info("flush/eviction write-back). Crash durability lives in the Database");
  Info("layer (WAL + recovery), which this demo does not link.");
  dirty_pool.reset();

  // ==================================================================
  Section("[7] ERROR HANDLING");
  // ==================================================================
  Info("Fallible calls return an explicit Status/DbError; the demo verifies");
  Info("both the error category and that pool state stays consistent.");

  std::unique_ptr<BufferPool> err_pool;
  Check("created a buffer pool with capacity 2",
        BufferPool::Create(storage.get(), 2, &err_pool).ok() &&
            err_pool != nullptr);
  {
    BufferPool::Handle h;
    Status st = err_pool->Fetch(Page::kInvalidId, &h);
    Info("invalid page id -> " + std::string(DbErrorString(st.code)));
    Check("fetch(invalid id) returns kInvalidArgument",
          st.code == DbError::kInvalidArgument);
    Check("pool unchanged after invalid-id fetch",
          !h.valid() && err_pool->size() == 0);
  }
  {
    BufferPool::Handle h;
    Status st = err_pool->Fetch(1000, &h);
    Info("page beyond end of storage -> " + std::string(DbErrorString(st.code)));
    Check("fetch beyond the end of storage returns kNotFound",
          st.code == DbError::kNotFound);
    Check("pool unchanged after beyond-end fetch",
          !h.valid() && err_pool->size() == 0);
  }
  {
    BufferPool::Handle a;
    BufferPool::Handle b;
    BufferPool::Handle fail;
    Check("pin both frames (pages 0 and 1)",
          err_pool->Fetch(0, &a).ok() && err_pool->Fetch(1, &b).ok());
    Status st = err_pool->Fetch(2, &fail);
    Info("all frames pinned -> " + std::string(DbErrorString(st.code)));
    Check("fetch with all frames pinned returns kBufferFull",
          st.code == DbError::kBufferFull);
    Check("failed fetch leaves its handle empty", !fail.valid());
    Check("the two pinned pages are still resident",
          err_pool->size() == 2 && err_pool->Contains(0) &&
              err_pool->Contains(1));
    a.Unpin();
    b.Unpin();
  }
  {
    Status st = err_pool->Resize(0);
    Info("invalid resize capacity -> " + std::string(DbErrorString(st.code)));
    Check("Resize(0) returns kInvalidArgument",
          st.code == DbError::kInvalidArgument);
    Check("capacity unchanged after invalid resize",
          err_pool->capacity() == 2);
  }
  {
    BufferPool::Handle a;
    BufferPool::Handle b;
    Check("pin both frames again",
          err_pool->Fetch(0, &a).ok() && err_pool->Fetch(1, &b).ok());
    Status st = err_pool->Resize(1);
    Info("shrink blocked by pins -> " + std::string(DbErrorString(st.code)));
    Check("shrink blocked by pinned pages returns kBufferFull",
          st.code == DbError::kBufferFull);
    Check("capacity and residency unchanged after blocked shrink",
          err_pool->capacity() == 2 && err_pool->Contains(0) &&
              err_pool->Contains(1));
    a.Unpin();
    b.Unpin();
  }
  err_pool.reset();

  // ==================================================================
  // Cleanup: close storage, then delete only the file this demo created.
  // ==================================================================
  std::printf("\n[CLEANUP]\n");
  storage->Close();
  storage.reset();
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
  const bool removed = !remove_error && !std::filesystem::exists(path);
  Check("temporary database file removed", removed);
  if (!removed) {
    Info("could not remove temporary file, please delete manually: " + path);
  }

  // ==================================================================
  Section("[8] FINAL VERIFICATION SUMMARY");
  // ==================================================================
  std::printf("    Checks executed: %d\n", g_checks);
  std::printf("    Passed:          %d\n", g_passed);
  std::printf("    Failed:          %d\n", g_failed);
  std::printf("    Overall result:  %s\n", g_failed == 0 ? "PASS" : "FAIL");

  std::printf("\n    Features demonstrated (backed by real checks above):\n");
  std::printf("      - Page storage: allocate, write, read, persistence across reopen\n");
  std::printf("      - Buffer-pool caching and residency tracking\n");
  std::printf("      - True LRU eviction with a capacity limit\n");
  std::printf("      - Dynamic buffer-pool growth, shrink, and pinned-page safety\n");
  std::printf("      - Pinning, move-only handle lifecycle, resize-safe handles\n");
  std::printf("      - Dirty-page write-back via flush and via eviction\n");
  std::printf("      - Explicit Status/DbError error handling\n");

  std::printf("\n    Out of scope for this buffer-pool demo (built in other layers):\n");
  std::printf("      - Record-level CRUD over SlottedPage/HeapTable/Catalog (R1)\n");
  std::printf("      - Write-ahead logging + crash recovery (R2: Database layer)\n");
  std::printf("      - Manager/Router coordination and the `nexusdb` CLI (R3)\n");
  std::printf("      - SQL parsing/execution, transactions (not implemented anywhere)\n");

  std::printf("\n    Limitations:\n");
  std::printf("      - This demo links only the storage + buffer-pool layers\n");
  std::printf("      - No cache-hit/disk-read counters are exposed by the API\n");
  std::printf("      - Single-threaded; no concurrency or locking\n");
  std::printf("      - Pages are allocated densely; no delete/free-list yet\n");

  std::printf("\n================================================================\n");
  std::printf("Overall result: %s (%d/%d checks passed)\n",
              g_failed == 0 ? "PASS" : "FAIL", g_passed, g_checks);
  std::printf("================================================================\n");

  return g_failed == 0 ? 0 : 1;
}
