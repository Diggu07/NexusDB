// NexusDB — fixed-capacity page buffer pool (Digvijay: Database Engine, Phase 2).
//
// The BufferPool caches a fixed number of Page objects in memory, called
// frames, on top of a PageStorage. PageStorage stays the ONLY component that
// touches the database file; the pool keeps hot pages resident and mediates
// all reads and writes through it.
//
// This is the Phase 2 foundation and deliberately has NO LRU policy yet.
// When the pool is full and a non-resident page is requested, the pool reuses
// the lowest-indexed unpinned frame. That replacement rule is a documented
// TEMPORARY, deterministic stand-in; the LRU policy replaces it in the next
// phase. Pinned frames are never evicted, under any circumstances.
//
// Ownership contract
// ------------------
//   - The BufferPool does NOT own the PageStorage. The storage must outlive
//     the pool and must stay open for the pool's lifetime. The pool must be
//     destroyed before its PageStorage.
//   - A Handle does NOT own the pool. Every Handle must be released (destroyed
//     or Unpin()ed) before its BufferPool is destroyed. Destroying the pool
//     while a Handle is still alive is a programming error: the Handle would
//     later dereference a destroyed pool.
//   - The pool owns every Page it caches. Callers never own a cached Page and
//     must NOT hold a raw Page pointer or reference beyond a live Handle.
//   - A caller reaches a cached page only through Handle: a small RAII guard
//     that pins the page when it is created and unpins it when it is
//     destroyed or moved-from. Because a pin is held, the frame cannot be
//     evicted while a Handle refers to it.
//   - Handle::page() is valid only while that Handle is valid. After the
//     Handle is destroyed, moved-from, or explicitly Unpin()ed, the frame may
//     be reused and the reference must no longer be used.
//   - A Handle is move-only; copying is deleted so exactly one guard owns each
//     pin. Pins are counted: several Handles may pin one page, and the page is
//     unpinned only when the last Handle releases it. Move construction and
//     move assignment both transfer the pin and leave the source empty;
//     move-assigning onto a Handle that already holds a pin releases the old
//     pin first, so no pin is ever leaked.
//   - Dirty state lives on the Page itself (Page::MarkDirty). A successful
//     FlushPage/FlushAll clears the page's dirty flag; a failed flush leaves
//     it dirty. A dirty page is flushed before its frame is reused, so a
//     successful operation never silently loses data.
//
// Error handling follows the project convention: every fallible call returns
// Status; no exception escapes the API.

#ifndef NEXUSDB_DB_BUFFER_POOL_H
#define NEXUSDB_DB_BUFFER_POOL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/error.h"
#include "db/page.h"

namespace nexusdb {

class PageStorage;

// A fixed-capacity cache of pages backed by PageStorage.
class BufferPool {
 public:
  // Sentinel frame index meaning "no frame".
  static constexpr std::size_t kNoFrame = static_cast<std::size_t>(-1);

  // RAII pin guard granting safe access to one cached page.
  //
  // Obtained from Fetch(). While a Handle is valid its page is pinned and
  // guaranteed resident. The page reference from page() is valid only for
  // the lifetime of the Handle.
  class Handle {
   public:
    Handle() = default;
    ~Handle();

    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    // True while this Handle still pins a page.
    bool valid() const { return pool_ != nullptr; }
    Page::PageId id() const {
      return pool_ != nullptr ? pool_->frames_[frame_].page.id() : Page::kInvalidId;
    }

    // Access to the pinned page. Precondition: valid().
    Page& page();
    const Page& page() const;

    bool IsDirty() const;
    void MarkDirty();

    // Release the pin early. Returns kInvalidArgument on an empty Handle
    // (e.g. a second call after it already released). Idempotent callers
    // should check valid() first.
    Status Unpin();

   private:
    friend class BufferPool;
    Handle(BufferPool* pool, std::size_t frame) : pool_(pool), frame_(frame) {}

    BufferPool* pool_ = nullptr;
    std::size_t frame_ = kNoFrame;
  };

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Creates a pool with `capacity` frames over `storage` (not owned).
  // Fails with kInvalidArgument when `storage` or `out` is null, or when
  // `capacity` is 0. On success `*out` owns the pool.
  static Status Create(PageStorage* storage,
                       std::size_t capacity,
                       std::unique_ptr<BufferPool>* out);

  // Best-effort FlushAll(); errors cannot be reported from a destructor, so
  // callers that care about durability must call FlushAll() themselves.
  ~BufferPool();

  // Fetches page `id`, reading it through PageStorage on a miss, and returns
  // a pinned Handle in `*out`. A resident page is returned without re-reading.
  // On failure `*out` is left empty. Errors:
  //   kInvalidArgument  null `out`, or `id` == Page::kInvalidId
  //   kBadState         pool has no storage
  //   kNotFound/kIoError propagated from PageStorage::ReadPage
  //   kBufferFull       every frame is pinned and the page is not resident
  Status Fetch(Page::PageId id, Handle* out);

  // Flushes page `id` if it is resident and dirty. kNotFound when `id` is not
  // resident (a non-resident page is already on disk, so there is nothing to
  // flush). kBadState when the pool has no storage. Errors from
  // PageStorage::WritePage are propagated and leave the page dirty.
  Status FlushPage(Page::PageId id);

  // Flushes every dirty resident page through PageStorage. Attempts all dirty
  // pages and returns the first error encountered, if any; a page whose flush
  // failed stays dirty. kBadState when the pool has no storage.
  Status FlushAll();

  // Configured frame capacity.
  std::size_t capacity() const { return capacity_; }

  // Number of currently resident pages.
  std::size_t size() const;

  // Number of frames that are currently pinned by at least one Handle.
  std::size_t pinned_frames() const;

  // True when page `id` is currently resident.
  bool Contains(Page::PageId id) const;

  // Number of pins currently held on resident page `id` (0 if not resident).
  std::size_t PinCount(Page::PageId id) const;

 private:
  BufferPool(PageStorage* storage, std::size_t capacity);

  struct Frame {
    Page page;
    std::size_t pin_count = 0;
    bool in_use = false;
  };

  // Index of the resident frame holding `id`, or kNoFrame.
  std::size_t FindFrame(Page::PageId id) const;

  // Chooses a frame to hold a page being loaded: a free frame if one exists,
  // otherwise the lowest-indexed unpinned frame (flushing it first if dirty).
  // Writes the chosen index to `*out`. Fails with kBufferFull when every frame
  // is pinned, or with the flush error when a victim could not be written.
  Status AcquireFrame(std::size_t* out);

  // Flushes frame `frame` if it is resident and dirty; clears the dirty flag
  // on success, keeps it on failure.
  Status FlushFrame(std::size_t frame);

  void UnpinFrame(std::size_t frame);

  PageStorage* storage_;
  std::size_t capacity_;
  std::vector<Frame> frames_;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_BUFFER_POOL_H
