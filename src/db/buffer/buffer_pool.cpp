// NexusDB — resizable page buffer pool implementation (Phase 2).
//
// Replacement is true LRU: a free frame is used first while the pool is below
// its logical capacity; otherwise the least recently used UNPINNED frame is
// reused, and a dirty victim is written back before reuse so no committed
// modification is lost. Recency is a per-frame monotonic access-clock stamp
// updated on every successful Fetch (hit or load). A resident page is always
// served from memory; a missing page is read through PageStorage exactly once.
//
// Frames live in a std::deque. Growing the pool therefore never moves existing
// frames, so Page references held through a live Handle stay valid across a
// Resize. capacity_ is the logical maximum resident-page count and may be less
// than frames_.size(): a shrink frees trailing frames but never relocates the
// survivors (which would invalidate index-based Handles).

#include "db/buffer_pool.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "db/page_storage.h"

namespace nexusdb {

BufferPool::BufferPool(PageStorage* storage, std::size_t capacity)
    : storage_(storage), capacity_(capacity), frames_(capacity) {}

BufferPool::~BufferPool() {
  // Best effort only: a destructor cannot report failure. Callers that need
  // to observe flush errors must call FlushAll() before destroying the pool.
  if (storage_ != nullptr) {
    FlushAll();
  }
}

Status BufferPool::Create(PageStorage* storage,
                          std::size_t capacity,
                          std::unique_ptr<BufferPool>* out) {
  if (storage == nullptr || out == nullptr) {
    return Status::Error(DbError::kInvalidArgument,
                         "null storage or null out");
  }
  if (capacity == 0) {
    return Status::Error(DbError::kInvalidArgument,
                         "buffer pool capacity must be positive");
  }

  std::unique_ptr<BufferPool> pool(new BufferPool(storage, capacity));
  *out = std::move(pool);
  return Status::Ok();
}

std::size_t BufferPool::FindFrame(Page::PageId id) const {
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i].in_use && frames_[i].page.id() == id) {
      return i;
    }
  }
  return kNoFrame;
}

std::uint64_t BufferPool::NextTimestamp() {
  if (access_clock_ == std::numeric_limits<std::uint64_t>::max()) {
    RenumberClock();
  }
  return ++access_clock_;
}

void BufferPool::RenumberClock() {
  // Compact the in-use frames' stamps to 1..n in recency order. Free frames
  // are ignored: their stale stamps are overwritten when next installed.
  std::vector<std::size_t> order;
  order.reserve(frames_.size());
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i].in_use) {
      order.push_back(i);
    }
  }
  std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
    return frames_[a].last_used < frames_[b].last_used;
  });
  std::uint64_t stamp = 0;
  for (std::size_t frame : order) {
    frames_[frame].last_used = ++stamp;
  }
  access_clock_ = stamp;
}

Status BufferPool::AcquireFrame(std::size_t* out) {
  // While below the logical capacity, prefer a free frame; nothing needs
  // evicting. The invariant frames_.size() >= capacity_ guarantees a free slot
  // exists here.
  if (size() < capacity_) {
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      if (!frames_[i].in_use) {
        *out = i;
        return Status::Ok();
      }
    }
  }
  // At capacity: evict the least recently used unpinned frame. Strict '<'
  // keeps the lowest-indexed frame on a tie, so the choice stays deterministic.
  std::size_t victim = kNoFrame;
  std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    const Frame& f = frames_[i];
    if (f.in_use && f.pin_count == 0 && f.last_used < oldest) {
      oldest = f.last_used;
      victim = i;
    }
  }
  if (victim == kNoFrame) {
    return Status::Error(DbError::kBufferFull,
                         "all frames are pinned; page cannot be loaded");
  }
  Status s = FlushFrame(victim);  // persist a dirty victim before reuse
  if (!s.ok()) {
    return s;  // victim and its dirty state are left untouched
  }
  *out = victim;
  return Status::Ok();
}

void BufferPool::TrimFreeFrames() {
  while (frames_.size() > capacity_ && !frames_.back().in_use) {
    frames_.pop_back();
  }
}

Status BufferPool::Resize(std::size_t new_capacity) {
  if (new_capacity == 0) {
    return Status::Error(DbError::kInvalidArgument,
                         "buffer pool capacity must be positive");
  }
  if (new_capacity == capacity_) {
    return Status::Ok();  // documented no-op
  }

  // The only allocations are frame storage for growth and the small temporary
  // vector used to rank shrink victims; both are reported as kOutOfMemory.
  try {
    if (new_capacity > capacity_) {
      // Grow. Allocate up front so allocation failure surfaces here rather
      // than at a later Fetch. Existing frames -- and therefore resident
      // pages, pins, dirty flags, and recency -- are preserved untouched.
      if (new_capacity > frames_.size()) {
        frames_.resize(new_capacity);
      }
      capacity_ = new_capacity;
      return Status::Ok();
    }

    // Shrink. Reduce the resident-page count to at most new_capacity by
    // evicting the LRU UNPINNED pages.
    const std::size_t residents = size();
    std::vector<std::size_t> victims;
    if (residents > new_capacity) {
      const std::size_t needed = residents - new_capacity;

      // Gather candidate victims (resident and unpinned) and check, before
      // changing anything, that enough of them exist.
      victims.reserve(needed);
      for (std::size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].in_use && frames_[i].pin_count == 0) {
          victims.push_back(i);
        }
      }
      if (victims.size() < needed) {
        return Status::Error(
            DbError::kBufferFull,
            "cannot shrink: not enough unpinned frames to evict");
      }

      // Rank LRU first (ties by index) and keep only as many as needed.
      // Inspection reads last_used only, so it never promotes a page.
      std::sort(victims.begin(), victims.end(),
                [this](std::size_t a, std::size_t b) {
                  if (frames_[a].last_used != frames_[b].last_used) {
                    return frames_[a].last_used < frames_[b].last_used;
                  }
                  return a < b;
                });
      victims.resize(needed);
    }

    // Write back dirty victims before touching any metadata. A failure aborts
    // the resize: capacity, resident pages, pins, and recency stay unchanged;
    // victims flushed before the failure are simply left clean, and the failing
    // victim stays dirty.
    for (std::size_t frame : victims) {
      Status s = FlushFrame(frame);
      if (!s.ok()) {
        return s;
      }
    }

    // Commit. Every victim is now clean, so releasing its slot cannot fail.
    for (std::size_t frame : victims) {
      frames_[frame] = Frame();
    }
    capacity_ = new_capacity;
    TrimFreeFrames();
    return Status::Ok();
  } catch (const std::bad_alloc&) {
    return Status::Error(DbError::kOutOfMemory,
                         "cannot allocate frame storage for resize");
  } catch (const std::length_error&) {
    return Status::Error(DbError::kOutOfMemory,
                         "requested buffer pool capacity is too large");
  }
}

Status BufferPool::Fetch(Page::PageId id, Handle* out) {
  if (out == nullptr) {
    return Status::Error(DbError::kInvalidArgument, "null handle pointer");
  }
  *out = Handle();
  if (storage_ == nullptr) {
    return Status::Error(DbError::kBadState, "buffer pool has no storage");
  }
  if (id == Page::kInvalidId) {
    return Status::Error(DbError::kInvalidArgument, "invalid page id");
  }

  const std::size_t resident = FindFrame(id);
  if (resident != kNoFrame) {
    frames_[resident].pin_count++;
    frames_[resident].last_used = NextTimestamp();  // a hit counts as an access
    *out = Handle(this, resident);
    return Status::Ok();
  }

  // Validate the id and read the page before disturbing any resident frame,
  // so a failed read leaves the pool's contents unchanged.
  Page page;
  Status s = storage_->ReadPage(id, &page);
  if (!s.ok()) {
    return s;
  }

  std::size_t slot = kNoFrame;
  s = AcquireFrame(&slot);
  if (!s.ok()) {
    return s;
  }

  Frame& frame = frames_[slot];
  frame.page = page;
  frame.pin_count = 1;
  frame.last_used = NextTimestamp();  // a freshly loaded page is most recent
  frame.in_use = true;
  *out = Handle(this, slot);
  return Status::Ok();
}

Status BufferPool::FlushFrame(std::size_t frame) {
  Frame& f = frames_[frame];
  if (!f.in_use || !f.page.IsDirty()) {
    return Status::Ok();
  }
  // Write-ahead rule: the log records needed to recover this page must be
  // durable before the page reaches storage. FlushFrame is the single choke
  // point for every dirty-page write (FlushPage, FlushAll, eviction in
  // AcquireFrame, and Resize shrink), so enforcing it here covers them all.
  if (log_sink_ != nullptr) {
    Status log_status = log_sink_->BeforePageWrite(f.page);
    if (!log_status.ok()) {
      return log_status;  // keep the page dirty; do not write data ahead of log
    }
  }
  Status s = storage_->WritePage(f.page);
  if (!s.ok()) {
    return s;  // leave the page dirty so the write can be retried
  }
  f.page.ClearDirty();
  return Status::Ok();
}

Status BufferPool::FlushPage(Page::PageId id) {
  if (storage_ == nullptr) {
    return Status::Error(DbError::kBadState, "buffer pool has no storage");
  }
  const std::size_t frame = FindFrame(id);
  if (frame == kNoFrame) {
    return Status::Error(DbError::kNotFound,
                         "page " + std::to_string(id) + " is not resident");
  }
  return FlushFrame(frame);
}

Status BufferPool::FlushAll() {
  if (storage_ == nullptr) {
    return Status::Error(DbError::kBadState, "buffer pool has no storage");
  }
  Status first_error = Status::Ok();
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (!frames_[i].in_use || !frames_[i].page.IsDirty()) {
      continue;
    }
    Status s = FlushFrame(i);
    if (!s.ok() && first_error.ok()) {
      first_error = s;
    }
  }
  return first_error;
}

std::size_t BufferPool::size() const {
  std::size_t count = 0;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i].in_use) {
      ++count;
    }
  }
  return count;
}

std::size_t BufferPool::pinned_frames() const {
  std::size_t count = 0;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i].in_use && frames_[i].pin_count > 0) {
      ++count;
    }
  }
  return count;
}

bool BufferPool::Contains(Page::PageId id) const {
  return FindFrame(id) != kNoFrame;
}

std::size_t BufferPool::PinCount(Page::PageId id) const {
  const std::size_t frame = FindFrame(id);
  return frame == kNoFrame ? 0 : frames_[frame].pin_count;
}

void BufferPool::UnpinFrame(std::size_t frame) {
  if (frame >= frames_.size()) {
    return;
  }
  Frame& f = frames_[frame];
  if (f.pin_count > 0) {
    f.pin_count--;
  }
}

// ---- Handle ---------------------------------------------------------------

BufferPool::Handle::~Handle() {
  if (pool_ != nullptr) {
    pool_->UnpinFrame(frame_);
  }
}

BufferPool::Handle::Handle(Handle&& other) noexcept
    : pool_(other.pool_), frame_(other.frame_) {
  other.pool_ = nullptr;
  other.frame_ = kNoFrame;
}

BufferPool::Handle& BufferPool::Handle::operator=(Handle&& other) noexcept {
  if (this != &other) {
    if (pool_ != nullptr) {
      pool_->UnpinFrame(frame_);
    }
    pool_ = other.pool_;
    frame_ = other.frame_;
    other.pool_ = nullptr;
    other.frame_ = kNoFrame;
  }
  return *this;
}

Page& BufferPool::Handle::page() {
  return pool_->frames_[frame_].page;
}

const Page& BufferPool::Handle::page() const {
  return pool_->frames_[frame_].page;
}

bool BufferPool::Handle::IsDirty() const {
  return pool_->frames_[frame_].page.IsDirty();
}

void BufferPool::Handle::MarkDirty() {
  pool_->frames_[frame_].page.MarkDirty();
}

Status BufferPool::Handle::Unpin() {
  if (pool_ == nullptr) {
    return Status::Error(DbError::kInvalidArgument, "handle is empty");
  }
  pool_->UnpinFrame(frame_);
  pool_ = nullptr;
  frame_ = kNoFrame;
  return Status::Ok();
}

}  // namespace nexusdb
