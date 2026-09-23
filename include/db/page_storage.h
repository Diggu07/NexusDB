// NexusDB — persistent page store (Digvijay: Database Engine, Phase 1).
//
// PageStorage maps fixed-size Pages onto a single database file:
//
//   page 0 | page 1 | page 2 | ...
//
// Every page occupies exactly kPageSize bytes; the file offset of page N
// is N * kPageSize. Pages are allocated densely from id 0 upward (next id
// = current file size / kPageSize). There is no free-page list and no
// deletion/reuse yet — those arrive with later phases.
//
// Rules:
//   - A page must be allocated (AllocatePage) before it can be written.
//     WritePage to an unallocated id returns kNotFound.
//   - WritePage is write-through: it flushes, so a successful return
//     means the bytes are in the OS. Close also flushes.
//   - PageStorage never touches Page dirty flags; dirty lifecycle is the
//     future Buffer Pool's job.
//   - Dependency direction: (future) Buffer Pool -> PageStorage -> Page.
//     PageStorage does not depend on any buffering, caching, SQL, WAL,
//     or transaction machinery.

#ifndef NEXUSDB_DB_PAGE_STORAGE_H
#define NEXUSDB_DB_PAGE_STORAGE_H

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "common/error.h"
#include "db/page.h"

namespace nexusdb {

class PageStorage {
 public:
  PageStorage(const PageStorage&) = delete;
  PageStorage& operator=(const PageStorage&) = delete;

  // Flushes (best effort) and releases the database file.
  ~PageStorage();

  // Opens the database file at `path`, creating it when missing.
  // Fails with kInvalidArgument (empty path) or kIoError (cannot
  // open/create, or an existing file whose size is not a multiple of
  // kPageSize). On success, `*out` owns the open storage.
  static Status Open(const std::string& path, std::unique_ptr<PageStorage>* out);

  // Reads page `id` into `*out` (reinitialized, clean flag). Fails with
  // kInvalidArgument (null `out`), kBadState (closed), kNotFound
  // (`id` beyond the last allocated page), or kIoError (seek/short read).
  Status ReadPage(Page::PageId id, Page* out);

  // Writes `page` to its on-disk slot and flushes. The page must have a
  // valid, previously allocated id, else kInvalidArgument/kNotFound.
  Status WritePage(const Page& page);

  // Appends a zeroed page; its id is the pre-allocation page count, so
  // the first page of an empty database is id 0. The file is extended
  // and flushed; the returned page is clean.
  Status AllocatePage(Page* out);

  // Number of allocated pages (file size / kPageSize).
  Status NumPages(std::uint32_t* out) const;

  // True when open and `id` refers to an allocated page. Never fails:
  // returns false when closed or on any I/O anomaly.
  bool PageExists(Page::PageId id) const;

  // Flushes buffered writes to the OS. kBadState when closed.
  Status Flush();

  // Flushes and closes. Idempotent: closing twice is kOk.
  Status Close();

  bool IsOpen() const { return open_; }
  const std::string& path() const { return path_; }

 private:
  explicit PageStorage(std::fstream file, std::string path);

  // File size in bytes; valid only when open. Negative on I/O failure.
  std::int64_t FileSize() const;

  mutable std::fstream file_;
  std::string path_;
  bool open_;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_PAGE_STORAGE_H
