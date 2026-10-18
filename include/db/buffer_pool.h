// NexusDB — resizable page buffer pool (Digvijay: Database Engine, Phase 2).
//
// The BufferPool caches Page objects in memory, called frames, on top of a
// PageStorage. PageStorage stays the ONLY component that touches the database
// file; the pool keeps hot pages resident and mediates all reads and writes
// through it.
//
// The pool is created with a capacity -- the maximum number of resident pages
// -- and that capacity can be changed at runtime with Resize(). Frame objects
// live in a std::deque so that growing the pool never moves existing frames:
// Page references obtained from a live Handle stay valid across a Resize.
//
// Replacement is true LRU (least recently used). When the pool is full and a
// non-resident page is requested, the least recently used UNPINNED frame is
// evicted; a dirty victim is written back first. Recency is tracked per frame
// with a monotonic access clock: every successful Fetch -- a cache hit or a
// newly loaded page -- stamps that frame as most recently used. Failed
// operations never advance recency. Pinned frames are never evicted; if every
// frame is pinned, Fetch fails with kBufferFull.
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
#include <deque>
#include <memory>

#include "common/error.h"
#include "db/page.h"

namespace nexusdb {

class PageStorage;

// Optional write-ahead-log barrier for the BufferPool.
//
// When installed on a pool, BeforePageWrite() is invoked immediately before a
// dirty page is written to PageStorage. Because EVERY persistence path funnels
// through BufferPool::FlushFrame -- an explicit FlushPage(), FlushAll(), LRU
// eviction in AcquireFrame(), and a Resize() shrink -- this single hook is the
// one place that can enforce the write-ahead rule: the page's redo log records
// must be durable before the page bytes reach storage.
//
// An implementation must make the page's log records durable before returning.
// A non-ok Status aborts the page write (the page stays dirty) and is
// propagated to the caller, so a failed log flush prevents the dependent data
// page from being written.
//
// The pool does NOT own the sink. A null sink (the default) preserves the
// legacy behavior of writing dirty pages directly with no logging.
class PageLogSink {
 public:
  virtual ~PageLogSink() = default;
  virtual Status BeforePageWrite(const Page& page) = 0;
};

// A resizable-capacity cache of pages backed by PageStorage.
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

  // Changes the maximum number of resident pages to `new_capacity`.
  // `new_capacity` must be positive (as with Create). Resizing to the current
  // capacity is a successful no-op: nothing is flushed, evicted, or has its
  // recency stamp changed.
  //
  // Increase: existing resident pages, pin counts, Handles, dirty flags, and
  //   relative LRU order are preserved and no page is flushed or evicted.
  //   Frame storage is allocated up front, so an allocation failure is
  //   reported here (kOutOfMemory) with the pool left unchanged.
  //
  // Decrease: the resident-page count is reduced to at most `new_capacity` by
  //   evicting the least recently used UNPINNED pages. Each dirty victim is
  //   written through PageStorage before it is removed. Pinned pages are never
  //   evicted and no outstanding Handle is invalidated.
  //
  // Failure atomicity: a decrease is committed only after every selected
  //   victim flushes successfully. If a victim's write fails, Resize returns
  //   that error and the pool keeps its original capacity and resident-page
  //   set; pages flushed before the failure are simply left clean (their bytes
  //   are already durable) while the failing victim stays dirty. A shrink that
  //   cannot reach the target because too many candidate frames are pinned
  //   fails with kBufferFull before anything is flushed. No Handle, pin count,
  //   dirty flag, or recency stamp is changed by a failed resize.
  //
  // Errors: kInvalidArgument (new_capacity == 0), kOutOfMemory (frame storage
  //   for a larger capacity could not be allocated), kBufferFull (a shrink
  //   cannot proceed because too many candidate frames are pinned), or a
  //   Status propagated from PageStorage::WritePage.
  Status Resize(std::size_t new_capacity);

  // Current maximum number of resident pages.
  std::size_t capacity() const { return capacity_; }

  // Number of currently resident pages.
  std::size_t size() const;

  // Number of frames that are currently pinned by at least one Handle.
  std::size_t pinned_frames() const;

  // True when page `id` is currently resident.
  bool Contains(Page::PageId id) const;

  // Number of pins currently held on resident page `id` (0 if not resident).
  std::size_t PinCount(Page::PageId id) const;

  // Install (or clear with nullptr) the write-ahead-log barrier described above.
  // The pool does not own the sink, which must outlive the pool. Setting a sink
  // affects only dirty-page writes performed AFTER this call.
  void SetLogSink(PageLogSink* sink) { log_sink_ = sink; }
  PageLogSink* log_sink() const { return log_sink_; }

 private:
  BufferPool(PageStorage* storage, std::size_t capacity);

  struct Frame {
    Page page;
    std::size_t pin_count = 0;
    std::uint64_t last_used = 0;  // access-clock stamp; higher == more recent
    bool in_use = false;
  };

  // Index of the resident frame holding `id`, or kNoFrame.
  std::size_t FindFrame(Page::PageId id) const;

  // Chooses a frame to hold a page being loaded. While the resident-page count
  // is below the logical capacity a free frame is used; only when the pool is
  // at capacity is the least recently used UNPINNED frame evicted (flushing it
  // first if dirty). Recency is read from Frame::last_used. Writes the chosen
  // index to `*out`. Fails with kBufferFull when every frame is pinned, or with
  // the flush error when a dirty victim could not be written (the victim is
  // then left intact so no data is lost).
  Status AcquireFrame(std::size_t* out);

  // Flushes frame `frame` if it is resident and dirty; clears the dirty flag
  // on success, keeps it on failure.
  Status FlushFrame(std::size_t frame);

  // Releases trailing free frames, but never below the logical capacity, so
  // the invariant `frames_.size() >= capacity_` always holds.
  void TrimFreeFrames();

  void UnpinFrame(std::size_t frame);

  // Returns the next monotonic access stamp (1, 2, 3, ...). If the 64-bit
  // clock would overflow, every in-use frame is renumbered in recency order
  // first, so recency can never wrap. Overflow needs 2^64 accesses and is
  // effectively unreachable; the guard keeps the ordering invariant
  // unconditional.
  std::uint64_t NextTimestamp();
  void RenumberClock();

  PageStorage* storage_;
  std::size_t capacity_;  // logical maximum resident pages
  // Physical frame slots. `frames_.size() >= capacity_` always holds; free
  // slots may exist beyond the logical capacity after a shrink. A deque keeps
  // existing frames (and any Handle::page() reference) valid when it grows.
  std::deque<Frame> frames_;
  std::uint64_t access_clock_ = 0;
  PageLogSink* log_sink_ = nullptr;  // optional write-ahead-log barrier
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_BUFFER_POOL_H
