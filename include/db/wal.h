// NexusDB — write-ahead log (Digvijay: Database Engine, R2).
//
// The WAL makes database-page modifications recoverable after an abrupt process
// termination. It is a small, append-only, self-describing log of full
// page-image records with a monotonic log sequence number (LSN) and a CRC-32
// checksum per record.
//
// Model: PHYSIOLOGICAL PAGE IMAGES (redo-only)
// -------------------------------------------
//   The smallest model that composes cleanly with this engine is a redo log of
//   full page images. A page image is captured at the moment the page is
//   flushed: BufferPool::FlushFrame calls Wal::BeforePageWrite(page) (via the
//   PageLogSink interface), which appends ONE page-image record holding the
//   page's current bytes and makes the log durable BEFORE the page itself is
//   written to PageStorage. Recovery replays the images in LSN order; the last
//   image for a page wins. This is idempotent (writing the same bytes again is
//   harmless) and reconstructs exactly what the buffer pool intended to write.
//
//   Consequences (documented limits):
//     - A modification is durable once its page image is durable in the WAL.
//       A dirty page that is never flushed (and therefore never logged) is not
//       recovered; that is expected, as it was never made durable.
//     - Page allocation is unaffected: PageStorage::AllocatePage is
//       write-through and runs BEFORE a fresh page can be modified, so every
//       page id appearing in the log is already allocated on disk.
//     - Starvation-free and lock-free: the WAL is a separate file with its own
//       stream; appending/flushing never touches the BufferPool, so no WAL/pool
//       deadlock is possible in this single-threaded engine.
//
// Durability boundary
// -------------------
//   "Durable" here means flushed to the operating system (std::fstream::flush),
//   exactly the guarantee PageStorage::WritePage documents. This survives a
//   process crash (including _exit / SIGKILL); it does NOT by itself survive an
//   OS crash or power loss, which would additionally require fsync. The WAL and
//   the database file share this boundary, so ordering between them is
//   preserved across process crashes.
//
// On-disk record format (little-endian), version 1
// ------------------------------------------------
//     offset  size  field
//     0       4     magic 'N','W','A','1'
//     4       2     version (uint16) = 1
//     6       2     type (uint16): 1 = PAGE_IMAGE
//     8       8     lsn (uint64): monotonically increasing, starts at 1
//     16      4     page_id (uint32)
//     20      4     payload_len (uint32): bytes after this header
//     24      4     reserved (uint32) = 0
//     28      4     checksum (uint32): CRC-32 (IEEE) over bytes [0,28) + payload
//     32      ...   payload (payload_len bytes; kPageSize for PAGE_IMAGE)
//
//   Records are appended whole and each Flush() makes all appended-but-not-yet-
//   durable records durable together. Because a record is written in one
//   append, the only partial record a crash can leave is the TRAILING one.
//
// Recovery / validation rules
// ---------------------------
//   Scanning stops cleanly at an incomplete trailing record:
//     - fewer than kHeaderSize bytes remain at the end of the file, or
//     - a header is present but fewer than its payload_len payload bytes remain.
//   A record whose complete header AND payload are both present is REQUIRED to
//   be valid; a bad magic/version/type, an out-of-range or non-page-size
//   payload_len, a non-increasing LSN, an invalid page id, or a checksum
//   mismatch is reported as kBadState (corruption), never silently skipped.
//   A torn 32-byte header of pure garbage is therefore reported as corruption
//   rather than treated as a torn tail; this errs toward failing loudly and is
//   the one theoretical false-positive window (documented limit).
//
//   Payload lengths are validated against kMaxPayload before any allocation, so
//   a corrupt length can never trigger an unbounded allocation.
//
// Errors use the project's Status/DbError convention; no exception escapes.

#ifndef NEXUSDB_DB_WAL_H
#define NEXUSDB_DB_WAL_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace nexusdb {

class Wal : public PageLogSink {
 public:
  // On-disk format constants (see the format table above). Exposed so the
  // format is inspectable and testable.
  static constexpr std::size_t kHeaderSize = 32;
  static constexpr std::uint16_t kVersion = 1;
  // Upper bound on any record payload; a corrupt length above this is rejected
  // before allocation.
  static constexpr std::uint32_t kMaxPayload = 64u * 1024u;
  // PAGE_IMAGE records always carry exactly one full page.
  static constexpr std::uint32_t kPageImagePayload =
      static_cast<std::uint32_t>(kPageSize);

  enum class RecordType : std::uint16_t { kPageImage = 1 };

  // Per-page replay statistics returned by Recover().
  struct RecoveryStats {
    std::uint64_t records_applied = 0;  // valid PAGE_IMAGE records replayed
    std::uint64_t last_lsn = 0;         // highest valid LSN seen (0 if none)
    bool truncated_tail = false;        // an incomplete trailing record was hit
  };

  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  ~Wal();

  // Opens (creating when missing) the log at `path` for append. An existing log
  // is scanned and validated: a mid-log corruption fails with kBadState,
  // while an incomplete trailing record is accepted and the next append resumes
  // after the last valid record. Fails with kInvalidArgument on null/empty
  // arguments or kIoError when the file cannot be opened.
  static Status Open(const std::string& path, std::unique_ptr<Wal>* out);

  // Flushes (best effort) and releases the log. Idempotent.
  Status Close();
  bool IsOpen() const { return open_; }
  const std::string& path() const { return path_; }

  // Appends one PAGE_IMAGE record for `page_id` with `len` bytes from `data`.
  // `len` must equal Page::size() and `data` must be non-null. Does NOT flush;
  // call Flush() (or BeforePageWrite) to make the record durable. On success
  // `*out_lsn` (when non-null) receives the record's LSN. Errors: kInvalidArgument
  // (bad args), kBadState (log closed), kIoError (append failed).
  Status AppendPageImage(Page::PageId page_id, const std::uint8_t* data,
                         std::size_t len, std::uint64_t* out_lsn = nullptr);

  // Makes every appended record durable. Errors: kBadState (closed), kIoError.
  Status Flush();

  // The LSN that the next appended record will use (1 for an empty log).
  std::uint64_t next_lsn() const { return next_lsn_; }

  // The highest LSN known to be durable (0 when nothing has been flushed).
  std::uint64_t durable_lsn() const { return durable_lsn_; }

  // PageLogSink: append a page image for `page` and flush, enforcing the
  // write-ahead rule. Propagates any append/flush error unchanged.
  Status BeforePageWrite(const Page& page) override;

  // Replays the log into `storage` by writing each PAGE_IMAGE record (in LSN
  // order) as a page. Idempotent: replaying twice yields identical bytes.
  // Stops cleanly at an incomplete trailing record (reported via
  // `stats->truncated_tail`); a fully-present but invalid record fails with
  // kBadState. A record naming an unallocated page is kBadState. `storage` must
  // be open and must outlive the call. Errors: kInvalidArgument (null storage),
  // kBadState (closed log or corruption), kIoError (read/write failure).
  Status Recover(PageStorage* storage, RecoveryStats* stats = nullptr) const;

  // Truncates the log to empty and resets LSN bookkeeping. Use after a
  // checkpoint (e.g. once recovery has been replayed and is durable). Errors:
  // kBadState (closed), kIoError.
  Status Reset();

 private:
  Wal(std::fstream file, std::string path);

  // Validates `file`'s contents without replaying. Sets `*out_last_lsn` to the
  // highest valid LSN and `*out_truncated` when an incomplete tail was seen.
  Status Scan(std::uint64_t* out_last_lsn, bool* out_truncated) const;

  std::fstream file_;
  std::string path_;
  bool open_;
  std::uint64_t next_lsn_ = 1;
  std::uint64_t durable_lsn_ = 0;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_WAL_H
