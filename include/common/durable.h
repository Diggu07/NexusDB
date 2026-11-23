// NexusDB — file durability helper (Digvijay: Database Engine).
//
// Synchronize a file's contents to durable storage without touching the
// caller's C++ stream. fsync() operates on the file's inode/page cache, so
// any open descriptor referring to the file may be used; this helper opens
// its own O_RDONLY descriptor, fsyncs it, and closes it. No stream internals
// are inspected, and no implementation-specific stream members are accessed.
//
// Durability contract:
//   - Userspace buffers (e.g. std::fstream::flush()) must be flushed first;
//     this function synchronizes the kernel page cache to the underlying
//     storage.
//   - On success the file's data and metadata are durable against an OS
//     crash / power loss, assuming the filesystem and storage honor fsync().
//   - This does not make newly created directory entries durable (that would
//     require fsync() on the parent directory fd); see the caller for when
//     that is needed.

#ifndef NEXUSDB_COMMON_DURABLE_H
#define NEXUSDB_COMMON_DURABLE_H

#include <cerrno>
#include <string>

#include "common/error.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nexusdb {

// Synchronize the file at `path` to durable storage.
// Returns kOk on success, kInvalidArgument on an empty path, or kIoError if
// the file cannot be opened or synchronization fails.
inline Status FsyncFile(const std::string& path) {
  if (path.empty()) {
    return Status::Error(DbError::kInvalidArgument, "empty path");
  }
#if defined(_WIN32)
  // Windows dev shim: _commit() flushes the C runtime buffers to the OS; the
  // real durability boundary on Linux is fsync() below. Note that Windows
  // FlushFileBuffers/_commit requires write access (_O_RDWR).
  const int fd = _open(path.c_str(), _O_RDWR | _O_BINARY);
  if (fd < 0) {
    return Status::Error(DbError::kIoError,
                         "cannot open file for synchronization: " + path);
  }
  const int rc = _commit(fd);
  const int saved_errno = errno;
  _close(fd);
  errno = saved_errno;
  if (rc != 0) {
    return Status::Error(DbError::kIoError,
                         "synchronization failed for file: " + path);
  }
  return Status::Ok();
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::Error(DbError::kIoError,
                         "cannot open file for synchronization: " + path);
  }
  const int rc = ::fsync(fd);
  const int saved_errno = errno;
  ::close(fd);
  errno = saved_errno;
  if (rc != 0) {
    return Status::Error(DbError::kIoError,
                         "synchronization failed for file: " + path);
  }
  return Status::Ok();
#endif
}

}  // namespace nexusdb

#endif  // NEXUSDB_COMMON_DURABLE_H
