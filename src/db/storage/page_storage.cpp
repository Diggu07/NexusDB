// NexusDB — PageStorage implementation (Phase 1: raw paged file I/O).
//
// All positioning uses put/get pointers reset per operation (with the
// stream state cleared first), so interleaved reads and writes on the
// single fstream stay correct. Every I/O result is checked; short
// reads/writes and seek/flush failures surface as kIoError.

#include "db/page_storage.h"

#include <cstdint>
#include <utility>

namespace nexusdb {
namespace {

// Byte offset of page `id`. Page ids are 32-bit and kPageSize is 4096,
// so the product always fits in 64 bits (max ~16 TiB database).
std::int64_t PageOffset(Page::PageId id) {
  return static_cast<std::int64_t>(id) * static_cast<std::int64_t>(kPageSize);
}

}  // namespace

PageStorage::PageStorage(std::fstream file, std::string path)
    : file_(std::move(file)), path_(std::move(path)), open_(true) {}

PageStorage::~PageStorage() {
  if (open_) {
    // Destructor cannot report errors: best-effort flush, then release.
    file_.flush();
    file_.close();
    open_ = false;
  }
}

Status PageStorage::Open(const std::string& path,
                         std::unique_ptr<PageStorage>* out) {
  if (path.empty() || out == nullptr) {
    return Status::Error(DbError::kInvalidArgument, "empty path or null out");
  }

  std::fstream file(path,
                    std::ios::in | std::ios::out | std::ios::binary);
  if (!file.is_open()) {
    // Missing file: create it, then reopen read/write.
    file.clear();
    file.open(path, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
      return Status::Error(DbError::kIoError,
                           "cannot create database file: " + path);
    }
    file.close();
    file.open(path, std::ios::in | std::ios::out | std::ios::binary);
  }
  if (!file.is_open()) {
    return Status::Error(DbError::kIoError,
                         "cannot open database file: " + path);
  }

  auto storage = std::unique_ptr<PageStorage>(
      new PageStorage(std::move(file), path));

  // A database file must be a whole number of pages; anything else is a
  // foreign/corrupt file — refuse it instead of silently misreading.
  const std::int64_t size = storage->FileSize();
  if (size < 0) {
    return Status::Error(DbError::kIoError,
                         "cannot stat database file: " + path);
  }
  if (size % static_cast<std::int64_t>(kPageSize) != 0) {
    return Status::Error(
        DbError::kIoError,
        "database file size is not a multiple of page size: " + path);
  }

  *out = std::move(storage);
  return Status::Ok();
}

Status PageStorage::ReadPage(Page::PageId id, Page* out) {
  if (out == nullptr) {
    return Status::Error(DbError::kInvalidArgument, "null page pointer");
  }
  if (!open_) {
    return Status::Error(DbError::kBadState, "storage is closed");
  }

  std::uint32_t count = 0;
  Status s = NumPages(&count);
  if (!s.ok()) {
    return s;
  }
  if (id >= count) {
    return Status::Error(DbError::kNotFound,
                         "page " + std::to_string(id) + " beyond end (" +
                             std::to_string(count) + " pages)");
  }

  file_.clear();
  file_.seekg(PageOffset(id), std::ios::beg);
  if (!file_) {
    return Status::Error(DbError::kIoError,
                         "seek failed for page " + std::to_string(id));
  }

  Page page(id);
  file_.read(reinterpret_cast<char*>(page.data()),
             static_cast<std::streamsize>(kPageSize));
  const std::streamsize got = file_.gcount();
  if (!file_ || got != static_cast<std::streamsize>(kPageSize)) {
    return Status::Error(DbError::kIoError,
                         "short read on page " + std::to_string(id) +
                             " (" + std::to_string(got) + " of " +
                             std::to_string(kPageSize) + " bytes)");
  }

  page.ClearDirty();  // freshly read from disk: matches on-disk state
  *out = page;
  return Status::Ok();
}

Status PageStorage::WritePage(const Page& page) {
  if (!page.IsValid()) {
    return Status::Error(DbError::kInvalidArgument, "invalid page id");
  }
  if (!open_) {
    return Status::Error(DbError::kBadState, "storage is closed");
  }

  std::uint32_t count = 0;
  Status s = NumPages(&count);
  if (!s.ok()) {
    return s;
  }
  // Writes only land on allocated pages; extending the file is
  // AllocatePage's job, so stray ids cannot punch holes in the file.
  if (page.id() >= count) {
    return Status::Error(DbError::kNotFound,
                         "page " + std::to_string(page.id()) +
                             " not allocated (" + std::to_string(count) +
                             " pages)");
  }

  file_.clear();
  file_.seekp(PageOffset(page.id()), std::ios::beg);
  if (!file_) {
    return Status::Error(DbError::kIoError,
                         "seek failed for page " + std::to_string(page.id()));
  }
  file_.write(reinterpret_cast<const char*>(page.data()),
              static_cast<std::streamsize>(kPageSize));
  file_.flush();
  if (!file_) {
    return Status::Error(DbError::kIoError,
                         "write failed on page " + std::to_string(page.id()));
  }
  // Note: the caller's dirty flag is left untouched — dirty lifecycle is
  // owned by the future Buffer Pool, not by storage.
  return Status::Ok();
}

Status PageStorage::AllocatePage(Page* out) {
  if (out == nullptr) {
    return Status::Error(DbError::kInvalidArgument, "null page pointer");
  }
  if (!open_) {
    return Status::Error(DbError::kBadState, "storage is closed");
  }

  std::uint32_t count = 0;
  Status s = NumPages(&count);
  if (!s.ok()) {
    return s;
  }
  if (count == Page::kInvalidId) {
    return Status::Error(DbError::kIoError, "page id space exhausted");
  }

  Page page(count);
  file_.clear();
  file_.seekp(PageOffset(count), std::ios::beg);
  if (!file_) {
    return Status::Error(DbError::kIoError, "seek failed on allocate");
  }
  file_.write(reinterpret_cast<const char*>(page.data()),
              static_cast<std::streamsize>(kPageSize));
  file_.flush();
  if (!file_) {
    return Status::Error(DbError::kIoError, "extend failed on allocate");
  }

  *out = page;  // zeroed, clean, id == pre-allocation page count
  return Status::Ok();
}

Status PageStorage::NumPages(std::uint32_t* out) const {
  if (out == nullptr) {
    return Status::Error(DbError::kInvalidArgument, "null count pointer");
  }
  if (!open_) {
    return Status::Error(DbError::kBadState, "storage is closed");
  }
  const std::int64_t size = FileSize();
  if (size < 0) {
    return Status::Error(DbError::kIoError, "cannot stat database file");
  }
  if (size % static_cast<std::int64_t>(kPageSize) != 0) {
    return Status::Error(DbError::kIoError,
                         "database file size changed unexpectedly");
  }
  *out = static_cast<std::uint32_t>(size / kPageSize);
  return Status::Ok();
}

bool PageStorage::PageExists(Page::PageId id) const {
  std::uint32_t count = 0;
  if (!open_ || !NumPages(&count).ok()) {
    return false;
  }
  return id < count;
}

Status PageStorage::Flush() {
  if (!open_) {
    return Status::Error(DbError::kBadState, "storage is closed");
  }
  file_.flush();
  if (!file_) {
    return Status::Error(DbError::kIoError, "flush failed");
  }
  return Status::Ok();
}

Status PageStorage::Close() {
  if (!open_) {
    return Status::Ok();  // idempotent
  }
  Status s = Flush();
  file_.close();
  open_ = false;
  return s;
}

std::int64_t PageStorage::FileSize() const {
  file_.clear();
  file_.seekg(0, std::ios::end);
  if (!file_) {
    return -1;
  }
  const std::streampos pos = file_.tellg();
  if (pos == std::streampos(-1)) {
    return -1;
  }
  return static_cast<std::int64_t>(pos);
}

}  // namespace nexusdb
