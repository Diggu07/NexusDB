// NexusDB — runnable BufferPool demonstration (Digvijay: Database Engine).
//
// A small, self-contained program that exercises the real Database Engine
// components -- PageStorage and BufferPool -- end to end:
//
//   1. LRU eviction: with a capacity-2 pool, the least recently used page is
//      evicted to make room for a new one.
//   2. Growing the pool at runtime (Resize up) without evicting residents.
//   3. Shrinking the pool at runtime (Resize down), evicting LRU pages.
//   4. A shrink that cannot run because too many frames are pinned is rejected
//      safely with kBufferFull, leaving the pool untouched.
//   5. Dirty-page persistence: pages flushed or evicted are written through
//      PageStorage and can be read back unchanged.
//
// This is a demonstration, not a test suite and not production code: it uses
// only the public DB engine API and does not implement SQL, WAL, or recovery.
// It prints PASS/FAIL for each check and exits non-zero if any check fails, so
// it can also be run under ctest. It works on a dedicated temporary database
// file and never touches any other database file.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
using nexusdb::Page;
using nexusdb::PageStorage;
using nexusdb::Status;

int g_failures = 0;

// Prints a single PASS/FAIL check and counts failures.
bool Check(const char* description, bool condition) {
  std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) {
    ++g_failures;
  }
  return condition;
}

// Fills a page with a single repeated byte and marks it dirty, so the rest of
// the demo can recognize the page's contents after a round-trip to disk.
void FillPattern(Page* page, std::uint8_t value) {
  std::memset(page->data(), value, Page::size());
  page->MarkDirty();
}

bool MatchesPattern(const Page& page, std::uint8_t value) {
  for (std::size_t i = 0; i < Page::size(); ++i) {
    if (page.data()[i] != value) {
      return false;
    }
  }
  return true;
}

// A unique, dedicated temporary database path for this run. The timestamp
// suffix guarantees it cannot collide with any existing file, so the demo
// never overwrites or deletes a user's database.
std::string DemoDbPath() {
  namespace fs = std::filesystem;
  const auto stamp =
      static_cast<long long>(std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count());
  const std::string name =
      "nexusdb_buffer_pool_demo_" + std::to_string(stamp) + ".db";
  return (fs::temp_directory_path() / name).string();
}

// Prints which of pages 0..page_count-1 are currently resident.
void PrintResident(const BufferPool& pool, int page_count, const char* when) {
  std::printf("       resident after %s:", when);
  for (int i = 0; i < page_count; ++i) {
    if (pool.Contains(static_cast<Page::PageId>(i))) {
      std::printf(" page%d", i);
    }
  }
  std::printf("\n");
}

}  // namespace

int main() {
  const std::string path = DemoDbPath();

  std::printf("NexusDB buffer-pool demonstration\n");
  std::printf("Temporary database file: %s\n", path.c_str());
  std::printf("Page size: %u bytes\n\n", nexusdb::kPageSize);

  std::unique_ptr<PageStorage> storage;
  Status status = PageStorage::Open(path, &storage);
  if (!status.ok() || storage == nullptr) {
    std::printf("FATAL: could not open demo database: %s\n",
                status.message.c_str());
    return 1;
  }

  // Setup: allocate pages 0..7, each holding a recognizable byte pattern.
  constexpr int kPageCount = 8;
  for (int i = 0; i < kPageCount; ++i) {
    Page page;
    if (!storage->AllocatePage(&page).ok()) {
      std::printf("FATAL: AllocatePage failed\n");
      return 1;
    }
    FillPattern(&page, static_cast<std::uint8_t>(0x10 + i));
    if (!storage->WritePage(page).ok()) {
      std::printf("FATAL: WritePage failed\n");
      return 1;
    }
  }
  std::printf(
      "Setup: allocated pages 0..%d, each prefilled with a distinct byte.\n",
      kPageCount - 1);

  // ---------------------------------------------------------------------
  // 1. LRU eviction with a capacity-2 pool.
  //    A(0), B(1), A(0) again, then C(2): B is the least recently used and
  //    must be evicted, while A and C stay resident.
  // ---------------------------------------------------------------------
  std::printf("\n1. LRU eviction with capacity 2\n");
  std::unique_ptr<BufferPool> pool;
  Check("create pool (capacity 2)",
        BufferPool::Create(storage.get(), 2, &pool).ok());
  if (pool == nullptr) {
    std::printf("FATAL: could not create buffer pool\n");
    return 1;
  }

  {
    BufferPool::Handle page_a;
    Check("fetch page 0 (A)", pool->Fetch(0, &page_a).ok());
  }
  {
    BufferPool::Handle page_b;
    Check("fetch page 1 (B)", pool->Fetch(1, &page_b).ok());
  }
  {
    BufferPool::Handle page_a;
    Check("fetch page 0 (A) again - cache hit, now most recent",
          pool->Fetch(0, &page_a).ok());
  }

  BufferPool::Handle page_c;
  Check("fetch page 2 (C) - pool full, must evict",
        pool->Fetch(2, &page_c).ok());
  Check("page 0 (A) still resident (recently used)", pool->Contains(0));
  Check("page 1 (B) was evicted (least recently used)", !pool->Contains(1));
  Check("page 2 (C) is resident", pool->Contains(2));
  Check("capacity is 2", pool->capacity() == 2);
  Check("resident count is 2", pool->size() == 2);
  page_c.Unpin();
  PrintResident(*pool, kPageCount, "LRU demo");

  // ---------------------------------------------------------------------
  // 2. Grow the pool: 2 -> 4. Existing residents survive; more pages can be
  //    loaded without evicting anything.
  // ---------------------------------------------------------------------
  std::printf("\n2. Growing the pool: 2 -> 4\n");
  Check("resize to 4 succeeds", pool->Resize(4).ok());
  Check("capacity is now 4", pool->capacity() == 4);
  Check("previous residents kept (0 and 2)",
        pool->Contains(0) && pool->Contains(2));
  {
    BufferPool::Handle page_b;
    Check("fetch page 1 (B)", pool->Fetch(1, &page_b).ok());
  }
  {
    BufferPool::Handle page_d;
    Check("fetch page 3 (D)", pool->Fetch(3, &page_d).ok());
  }
  Check("four pages now resident, none evicted", pool->size() == 4);
  Check("pages 0,1,2,3 all resident",
        pool->Contains(0) && pool->Contains(1) && pool->Contains(2) &&
            pool->Contains(3));
  PrintResident(*pool, kPageCount, "grow demo");

  // ---------------------------------------------------------------------
  // 3. Shrink the pool: 4 -> 2. Promote pages 2 and 3 first, so the LRU
  //    victims are 0 and 1 -- proving selection is by recency, not by id.
  // ---------------------------------------------------------------------
  std::printf("\n3. Shrinking the pool: 4 -> 2 (LRU survivors)\n");
  {
    BufferPool::Handle page_c;
    Check("touch page 2 (promote to most recent)", pool->Fetch(2, &page_c).ok());
  }
  {
    BufferPool::Handle page_d;
    Check("touch page 3 (promote to most recent)", pool->Fetch(3, &page_d).ok());
  }
  Check("resize to 2 succeeds", pool->Resize(2).ok());
  Check("capacity is now 2", pool->capacity() == 2);
  Check("resident count is 2", pool->size() == 2);
  Check("most recently used pages 2 and 3 survived",
        pool->Contains(2) && pool->Contains(3));
  Check("least recently used pages 0 and 1 evicted",
        !pool->Contains(0) && !pool->Contains(1));
  PrintResident(*pool, kPageCount, "shrink demo");
  pool.reset();

  // ---------------------------------------------------------------------
  // 4. A shrink that would have to evict pinned pages is rejected safely.
  // ---------------------------------------------------------------------
  std::printf("\n4. Shrinking with pinned pages is rejected safely\n");
  std::unique_ptr<BufferPool> pinned_pool;
  Check("create pool (capacity 3)",
        BufferPool::Create(storage.get(), 3, &pinned_pool).ok());
  if (pinned_pool == nullptr) {
    std::printf("FATAL: could not create pinned pool\n");
    return 1;
  }
  BufferPool::Handle pin0;
  BufferPool::Handle pin1;
  BufferPool::Handle pin2;
  Check("pin page 4", pinned_pool->Fetch(4, &pin0).ok());
  Check("pin page 5", pinned_pool->Fetch(5, &pin1).ok());
  Check("pin page 6", pinned_pool->Fetch(6, &pin2).ok());
  Check("all three frames are pinned", pinned_pool->pinned_frames() == 3);

  Status shrink = pinned_pool->Resize(1);
  Check("resize to 1 is rejected", !shrink.ok());
  Check("rejection code is kBufferFull",
        shrink.code == DbError::kBufferFull);
  Check("capacity unchanged (3)", pinned_pool->capacity() == 3);
  Check("pinned pages still resident",
        pinned_pool->Contains(4) && pinned_pool->Contains(5) &&
            pinned_pool->Contains(6));
  Check("pin counts unchanged",
        pinned_pool->PinCount(4) == 1 && pinned_pool->PinCount(5) == 1 &&
            pinned_pool->PinCount(6) == 1);
  Check("handles remain valid",
        pin0.valid() && pin1.valid() && pin2.valid());
  pin0.Unpin();
  pin1.Unpin();
  pin2.Unpin();
  pinned_pool.reset();

  // ---------------------------------------------------------------------
  // 5. Dirty-page persistence. A flushed page, and a dirty page written back
  //    by eviction, are both readable from storage afterwards.
  // ---------------------------------------------------------------------
  std::printf("\n5. Dirty-page persistence through PageStorage\n");
  std::unique_ptr<BufferPool> dirty_pool;
  Check("create pool (capacity 2)",
        BufferPool::Create(storage.get(), 2, &dirty_pool).ok());
  if (dirty_pool == nullptr) {
    std::printf("FATAL: could not create dirty pool\n");
    return 1;
  }

  {
    BufferPool::Handle page;
    Check("fetch page 5", dirty_pool->Fetch(5, &page).ok());
    FillPattern(&page.page(), 0xAB);
    Check("page 5 marked dirty", page.IsDirty());
    Check("flush page 5 succeeds", dirty_pool->FlushPage(5).ok());
    Check("page 5 clean after flush", !page.IsDirty());
  }
  Page reloaded;
  Check("read page 5 back from storage", storage->ReadPage(5, &reloaded).ok());
  Check("flushed bytes persisted (0xAB)", MatchesPattern(reloaded, 0xAB));

  {
    BufferPool::Handle page;
    Check("fetch page 6", dirty_pool->Fetch(6, &page).ok());
    FillPattern(&page.page(), 0xCD);
    Check("page 6 marked dirty", page.IsDirty());
  }
  {
    BufferPool::Handle page;
    Check("touch page 5 so page 6 becomes least recent",
          dirty_pool->Fetch(5, &page).ok());
  }
  {
    BufferPool::Handle page;
    Check("fetch page 7 - evicts dirty page 6, writing it back",
          dirty_pool->Fetch(7, &page).ok());
  }
  Check("page 6 was evicted", !dirty_pool->Contains(6));
  Page evicted;
  Check("read page 6 back from storage", storage->ReadPage(6, &evicted).ok());
  Check("evicted dirty bytes persisted (0xCD)", MatchesPattern(evicted, 0xCD));
  dirty_pool.reset();

  // ---------------------------------------------------------------------
  // Cleanup: pools must be destroyed before storage, then the temporary
  // database file is removed.
  // ---------------------------------------------------------------------
  std::printf("\nCleanup\n");
  pool.reset();
  pinned_pool.reset();
  dirty_pool.reset();
  storage->Close();
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
  Check("temporary database removed", !remove_error);

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "DEMONSTRATION OK" : "DEMONSTRATION FAILED",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
