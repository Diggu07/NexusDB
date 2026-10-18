// NexusDB — database open/recovery facade implementation (R2).
//
// See include/db/database.h for the enforced startup sequence and the database
// header format. Recovery is performed against raw PageStorage before the
// BufferPool exists, so the pool never observes a pre-recovery page.

#include "db/database.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace nexusdb {
namespace {

constexpr std::uint8_t kMagic0 = 0x4Eu;  // 'N'
constexpr std::uint8_t kMagic1 = 0x44u;  // 'D'
constexpr std::uint8_t kMagic2 = 0x42u;  // 'B'
constexpr std::uint8_t kMagic3 = 0x31u;  // '1'

constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffCatalogRoot = 8;

inline Status Bad(const char* message) {
  return Status::Error(DbError::kInvalidArgument, message);
}

std::uint16_t ReadU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(p[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

void WriteU16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void WriteU32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

// Test-only crash injection for the creation boundaries. Inert unless the
// NEXUSDB_CRASH_AT environment variable names the current point; when it does,
// the process terminates immediately (no unwinding), exactly as an abrupt crash
// would, so the tests can verify that a reopen after an interrupted creation is
// safe. A no-op off POSIX.
void CrashPoint(const char* point) {
#if defined(__unix__) || defined(__APPLE__)
  const char* want = std::getenv("NEXUSDB_CRASH_AT");
  if (want != nullptr && std::strcmp(want, point) == 0) {
    _exit(70);
  }
#else
  (void)point;
#endif
}

}  // namespace

Database::~Database() = default;

Status Database::ReadHeader(PageStorage* storage,
                            Page::PageId* out_catalog_root) {
  Page header;
  Status s = storage->ReadPage(0, &header);
  if (!s.ok()) {
    return s;
  }
  const std::uint8_t* d = header.data();
  if (d[kOffMagic + 0] != kMagic0 || d[kOffMagic + 1] != kMagic1 ||
      d[kOffMagic + 2] != kMagic2 || d[kOffMagic + 3] != kMagic3) {
    return Status::Error(DbError::kBadState,
                         "database: header page has bad magic");
  }
  if (ReadU16(d + kOffVersion) != kHeaderVersion) {
    return Status::Error(DbError::kBadState,
                         "database: unsupported header version");
  }
  const Page::PageId root = ReadU32(d + kOffCatalogRoot);
  if (root == Page::kInvalidId) {
    return Status::Error(DbError::kBadState,
                         "database: header has no catalog root");
  }
  *out_catalog_root = root;
  return Status::Ok();
}

Status Database::WriteHeader(PageStorage* storage,
                             Page::PageId catalog_root) {
  Page header(0);
  std::uint8_t* d = header.data();
  std::memset(d, 0, Page::size());
  d[kOffMagic + 0] = kMagic0;
  d[kOffMagic + 1] = kMagic1;
  d[kOffMagic + 2] = kMagic2;
  d[kOffMagic + 3] = kMagic3;
  WriteU16(d + kOffVersion, kHeaderVersion);
  WriteU32(d + kOffCatalogRoot, catalog_root);
  return storage->WritePage(header);
}

Status Database::CreateFreshDatabaseAtomically(const std::string& db_path,
                                               const std::string& wal_path,
                                               std::size_t pool_capacity) {
  const std::string tmp_db = db_path + ".creating";
  std::error_code ec;
  std::filesystem::remove(tmp_db, ec);  // discard a prior interrupted attempt

  auto abandon = [&tmp_db]() {
    std::error_code e;
    std::filesystem::remove(tmp_db, e);
  };

  // 1. Build a complete, self-consistent image at the temporary path.
  {
    std::unique_ptr<PageStorage> storage;
    Status s = PageStorage::Open(tmp_db, &storage);
    if (!s.ok()) {
      abandon();
      return s;
    }
    Page header;
    s = storage->AllocatePage(&header);  // reserve page 0 as the header
    if (!s.ok()) {
      storage->Close();
      abandon();
      return s;
    }
    CrashPoint("after-temp-page0");

    std::unique_ptr<BufferPool> pool;
    s = BufferPool::Create(storage.get(), pool_capacity, &pool);
    if (!s.ok()) {
      storage->Close();
      abandon();
      return s;
    }
    Page::PageId catalog_root = Page::kInvalidId;
    std::unique_ptr<QueryExecutor> executor;
    s = QueryExecutor::Create(storage.get(), pool.get(), &executor,
                              &catalog_root);
    if (!s.ok()) {
      storage->Close();
      abandon();
      return s;
    }
    CrashPoint("after-catalog");

    s = WriteHeader(storage.get(), catalog_root);
    if (!s.ok()) {
      storage->Close();
      abandon();
      return s;
    }
    CrashPoint("after-header");

    s = storage->Close();
    if (!s.ok()) {
      abandon();
      return s;
    }
  }

  // 2. A brand-new database has no redo history. Drop any stale log BEFORE the
  //    publish, so a crash can never pair the published database with a foreign
  //    WAL. Removing an absent log is a no-op.
  std::error_code wal_ec;
  std::filesystem::remove(wal_path, wal_ec);
  if (wal_ec) {
    abandon();
    return Status::Error(DbError::kIoError,
                         "database: cannot clear stale wal: " + wal_path);
  }
  CrashPoint("before-publish");

  // 3. Publish atomically. The target is absent or zero-length here; removing it
  //    first keeps the rename valid on both POSIX and Windows.
  std::error_code rm_ec;
  std::filesystem::remove(db_path, rm_ec);
  if (rm_ec) {
    abandon();
    return Status::Error(DbError::kIoError,
                         "database: cannot replace placeholder: " + db_path);
  }
  std::filesystem::rename(tmp_db, db_path, ec);
  if (ec) {
    abandon();
    return Status::Error(DbError::kIoError,
                         "database: cannot publish new database: " + db_path);
  }
  CrashPoint("after-publish");
  return Status::Ok();
}

Status Database::Open(const std::string& db_path, const std::string& wal_path,
                      std::size_t pool_capacity,
                      std::unique_ptr<Database>* out) {
  if (db_path.empty() || wal_path.empty() || out == nullptr) {
    return Bad("Database::Open: empty path or null out");
  }
  if (pool_capacity == 0) {
    return Bad("Database::Open: pool capacity must be positive");
  }

  // Only a missing or zero-length database file is treated as new. Anything
  // else is opened below and its header validated, so a foreign or corrupt file
  // fails loudly instead of being silently overwritten.
  std::error_code ec;
  bool fresh = !std::filesystem::exists(db_path, ec);
  if (ec) {
    return Status::Error(DbError::kIoError,
                         "database: cannot stat database file: " + db_path);
  }
  if (!fresh) {
    const std::uintmax_t bytes = std::filesystem::file_size(db_path, ec);
    if (ec) {
      return Status::Error(DbError::kIoError,
                           "database: cannot stat database file: " + db_path);
    }
    fresh = (bytes == 0);
  }
  if (fresh) {
    Status s = CreateFreshDatabaseAtomically(db_path, wal_path, pool_capacity);
    if (!s.ok()) {
      return s;
    }
  } else {
    // Best-effort: clear an image abandoned by an earlier interrupted creation
    // (possible only if this database file was published afterwards).
    std::error_code tmp_ec;
    std::filesystem::remove(db_path + ".creating", tmp_ec);
  }

  std::unique_ptr<PageStorage> storage;
  Status s = PageStorage::Open(db_path, &storage);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<Wal> wal;
  s = Wal::Open(wal_path, &wal);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<Database> db(new Database());
  db->storage_ = std::move(storage);
  db->wal_ = std::move(wal);

  Page::PageId catalog_root = Page::kInvalidId;
  s = ReadHeader(db->storage_.get(), &catalog_root);
  if (!s.ok()) {
    return s;
  }
  // Replay the log BEFORE creating the pool, so no stale page can be cached.
  s = db->wal_->Recover(db->storage_.get(), nullptr);
  if (!s.ok()) {
    return s;
  }
  // Replayed images are durable (WritePage is write-through); checkpoint.
  s = db->wal_->Reset();
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<BufferPool> pool;
  s = BufferPool::Create(db->storage_.get(), pool_capacity, &pool);
  if (!s.ok()) {
    return s;
  }
  pool->SetLogSink(db->wal_.get());

  std::unique_ptr<QueryExecutor> executor;
  s = QueryExecutor::Open(db->storage_.get(), pool.get(), catalog_root,
                          &executor);
  if (!s.ok()) {
    return s;
  }

  db->pool_ = std::move(pool);
  db->executor_ = std::move(executor);
  db->catalog_root_ = catalog_root;
  *out = std::move(db);
  return Status::Ok();
}

Status Database::Close() {
  if (storage_ == nullptr || !storage_->IsOpen()) {
    return Status::Ok();
  }
  executor_.reset();

  Status first = Status::Ok();
  if (pool_ != nullptr) {
    first = pool_->FlushAll();  // write-ahead ordered via the log sink
    if (!first.ok()) {
      return first;
    }
  }
  pool_.reset();

  if (wal_ != nullptr) {
    Status s = wal_->Reset();  // checkpoint: everything durable now
    if (!s.ok()) {
      return s;
    }
    s = wal_->Close();
    if (!s.ok()) {
      return s;
    }
  }
  return storage_->Close();
}

}  // namespace nexusdb
