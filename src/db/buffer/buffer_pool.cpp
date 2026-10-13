// NexusDB — fixed-capacity page buffer pool implementation (Phase 2).
//
// Replacement is intentionally simple and deterministic in this phase: the
// lowest-indexed unpinned frame is reused (free frames first). LRU replaces
// this in the next phase. A resident page is always served from memory; a
// missing page is read through PageStorage exactly once. Dirty victims are
// written back before reuse, so no committed modification is lost.

#include "db/buffer_pool.h"

#include <string>
#include <utility>

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
  for (std::size_t i = 0; i < capacity_; ++i) {
    if (frames_[i].in_use && frames_[i].page.id() == id) {
      return i;
    }
  }
  return kNoFrame;
}

Status BufferPool::AcquireFrame(std::size_t* out) {
  // Prefer a never-used frame.
  for (std::size_t i = 0; i < capacity_; ++i) {
    if (!frames_[i].in_use) {
      *out = i;
      return Status::Ok();
    }
  }
  // Otherwise reuse the lowest-indexed unpinned frame (temporary policy).
  for (std::size_t i = 0; i < capacity_; ++i) {
    if (frames_[i].pin_count == 0) {
      Status s = FlushFrame(i);  // persist a dirty victim before reuse
      if (!s.ok()) {
        return s;
      }
      *out = i;
      return Status::Ok();
    }
  }
  return Status::Error(DbError::kBufferFull,
                       "all frames are pinned; page cannot be loaded");
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
  frame.in_use = true;
  *out = Handle(this, slot);
  return Status::Ok();
}

Status BufferPool::FlushFrame(std::size_t frame) {
  Frame& f = frames_[frame];
  if (!f.in_use || !f.page.IsDirty()) {
    return Status::Ok();
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
  for (std::size_t i = 0; i < capacity_; ++i) {
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
  for (std::size_t i = 0; i < capacity_; ++i) {
    if (frames_[i].in_use) {
      ++count;
    }
  }
  return count;
}

std::size_t BufferPool::pinned_frames() const {
  std::size_t count = 0;
  for (std::size_t i = 0; i < capacity_; ++i) {
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
  if (frame >= capacity_) {
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
