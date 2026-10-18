// NexusDB — database open/recovery facade (Digvijay: Database Engine, R2).
//
// Database is the narrow owner that ties the storage stack together and, most
// importantly, runs write-ahead-log recovery BEFORE any normal reads or writes
// can occur. It owns the PageStorage, the Wal, the BufferPool, and the
// QueryExecutor in the one order that keeps their lifetimes and the crash
// protocol correct.
//
// Startup sequence (enforced, in this order)
// ------------------------------------------
//   1. Open the database file (PageStorage).
//   2. Open and VALIDATE the log (Wal::Open scans and rejects corruption).
//   3. If the database already holds data, read the database header page
//      (page 0) for the catalog root, then REPLAY the log into storage
//      (Wal::Recover). This runs before the BufferPool exists, so the pool can
//      never cache a stale (pre-recovery) page.
//   4. Checkpoint: the replayed images are already durable (PageStorage::WritePage
//      is write-through), so the log is truncated (Wal::Reset).
//   5. Create the BufferPool and attach the log as its write-ahead barrier, then
//      open the Catalog/QueryExecutor over the recovered file.
//   6. Normal operations may now proceed.
//
//   On a brand-new database the file is empty. It is created crash-atomically:
//   the complete image (page 0 header + a fresh catalog) is built at a
//   temporary path `<db_path>.creating` in the same directory and then
//   published with a single `rename`. A crash at any point therefore leaves the
//   real database path either absent/empty (so the next Open simply builds it
//   again) or the fully published database -- never a valid-looking file that
//   cannot be recovered. Any stale write-ahead log is discarded before the
//   publish, so a published database can never be paired with a foreign log.
//
//   Creation atomicity is an OS-level (namespace/rename) guarantee. It survives
//   a process crash; like the rest of this engine it does NOT by itself survive
//   an OS crash or power loss, which would additionally require directory
//   fsync (out of scope here).
//
// Steady-state durability
// -----------------------
//   Every dirty-page write goes through BufferPool::FlushFrame, which calls
//   Wal::BeforePageWrite() to append and flush the page image before the page
//   is written. A clean Close() flushes all dirty pages (WAL-ordered), resets
//   the log and closes everything. An abrupt termination leaves the log on disk;
//   the next Open replays it.
//
// Database header page (page 0, magic 'N','D','B','1')
// ---------------------------------------------------
//     offset  size  field
//     0       4     magic 'N','D','B','1'
//     4       2     version (uint16) = 1
//     6       2     reserved = 0
//     8       4     catalog_root (uint32)
//     12      ...   reserved (zero)
//
// Errors use the project's Status/DbError convention; no exception escapes.

#ifndef NEXUSDB_DB_DATABASE_H
#define NEXUSDB_DB_DATABASE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/query_executor.h"
#include "db/wal.h"

namespace nexusdb {

class Database {
 public:
  static constexpr std::uint16_t kHeaderVersion = 1;

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Flushes (best effort) and releases all owned resources.
  ~Database();

  // Opens (creating when missing) the database at `db_path` with its write-ahead
  // log at `wal_path`, performing recovery before returning. `pool_capacity` is
  // the BufferPool frame count (must be positive). Fails with kInvalidArgument on
  // bad arguments, kIoError from the storage/log, or kBadState when the database
  // header or log is corrupt / a replayed image is inconsistent.
  static Status Open(const std::string& db_path, const std::string& wal_path,
                     std::size_t pool_capacity,
                     std::unique_ptr<Database>* out);

  // Flushes all dirty pages (write-ahead ordered), checkpoints and closes the
  // log, then closes storage. Idempotent-ish: safe to call once; a second call
  // is a no-op. Reports the first error encountered.
  Status Close();

  // Normal-operation surface (valid only while open/recovered). Not null after
  // a successful Open.
  QueryExecutor& executor() { return *executor_; }
  PageStorage* storage() { return storage_.get(); }
  BufferPool* pool() { return pool_.get(); }
  Wal* wal() { return wal_.get(); }

  // Catalog root page id recorded in the database header.
  Page::PageId catalog_root() const { return catalog_root_; }

 private:
  Database() = default;

  static Status ReadHeader(PageStorage* storage, Page::PageId* out_catalog_root);
  static Status WriteHeader(PageStorage* storage, Page::PageId catalog_root);

  // Builds a complete new database image at `<db_path>.creating`, discards any
  // stale log at `wal_path`, and atomically publishes it as `db_path`. Only
  // called when `db_path` is absent or zero-length.
  static Status CreateFreshDatabaseAtomically(const std::string& db_path,
                                              const std::string& wal_path,
                                              std::size_t pool_capacity);

  std::unique_ptr<PageStorage> storage_;
  std::unique_ptr<Wal> wal_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<QueryExecutor> executor_;
  Page::PageId catalog_root_ = Page::kInvalidId;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_DATABASE_H
