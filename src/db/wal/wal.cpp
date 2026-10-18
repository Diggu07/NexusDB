// NexusDB — write-ahead log implementation (R2).
//
// See include/db/wal.h for the record format, the recovery model and the
// documented durability boundary. The log is a separate append-only file with
// its own stream; it never touches the BufferPool or PageStorage on the append
// path, so it cannot deadlock against them.

#include "db/wal.h"

#include <cstring>
#include <vector>

namespace nexusdb {
namespace {

constexpr std::uint8_t kMagic0 = 0x4Eu;  // 'N'
constexpr std::uint8_t kMagic1 = 0x57u;  // 'W'
constexpr std::uint8_t kMagic2 = 0x41u;  // 'A'
constexpr std::uint8_t kMagic3 = 0x31u;  // '1'

constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffType = 6;
constexpr std::size_t kOffLsn = 8;
constexpr std::size_t kOffPageId = 16;
constexpr std::size_t kOffPayloadLen = 20;
constexpr std::size_t kOffChecksum = 28;

inline Status Bad(const char* message) {
  return Status::Error(DbError::kInvalidArgument, message);
}

inline Status BadState(const char* message) {
  return Status::Error(DbError::kBadState, message);
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

std::uint64_t ReadU64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | p[i];
  }
  return v;
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

void WriteU64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
  }
}

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), computed incrementally
// so the header and payload can be folded in separately. Deterministic and
// dependency-free.
std::uint32_t Crc32Update(std::uint32_t crc, const std::uint8_t* data,
                          std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

std::uint32_t Crc32(const std::uint8_t* header, const std::uint8_t* payload,
                    std::uint32_t payload_len) {
  std::uint32_t crc = Crc32Update(0xFFFFFFFFu, header, kOffChecksum);
  crc = Crc32Update(crc, payload, payload_len);
  return crc ^ 0xFFFFFFFFu;
}

// Result of reading one record from a log stream.
enum class ReadResult {
  kRecord,   // a complete, valid record was read into `*rec`
  kEnd,      // clean end of log (possibly after an incomplete trailing record)
  kCorrupt,  // a complete record was present but invalid
};

struct ParsedRecord {
  std::uint16_t type = 0;
  std::uint64_t lsn = 0;
  Page::PageId page_id = Page::kInvalidId;
  std::uint32_t payload_len = 0;
  std::vector<std::uint8_t> payload;
};

// Reads and validates one record. Stops at an incomplete tail (kEnd with
// `*truncated` set) and reports a fully-present-but-invalid record as
// kCorrupt with a message in `*err`.
ReadResult ReadRecord(std::istream& in, ParsedRecord* rec, std::string* err,
                      bool* truncated) {
  std::uint8_t header[Wal::kHeaderSize];
  in.read(reinterpret_cast<char*>(header),
          static_cast<std::streamsize>(Wal::kHeaderSize));
  std::streamsize got = in.gcount();
  if (got == 0) {
    return ReadResult::kEnd;  // clean EOF, nothing pending
  }
  if (got < static_cast<std::streamsize>(Wal::kHeaderSize)) {
    *truncated = true;  // a partially written trailing header
    return ReadResult::kEnd;
  }

  const bool magic_ok = header[kOffMagic + 0] == kMagic0 &&
                        header[kOffMagic + 1] == kMagic1 &&
                        header[kOffMagic + 2] == kMagic2 &&
                        header[kOffMagic + 3] == kMagic3;
  if (!magic_ok) {
    *err = "wal: record has bad magic";
    return ReadResult::kCorrupt;
  }
  if (ReadU16(header + kOffVersion) != Wal::kVersion) {
    *err = "wal: unsupported record version";
    return ReadResult::kCorrupt;
  }
  const std::uint16_t type = ReadU16(header + kOffType);
  if (type != static_cast<std::uint16_t>(Wal::RecordType::kPageImage)) {
    *err = "wal: unknown record type";
    return ReadResult::kCorrupt;
  }
  const std::uint32_t payload_len = ReadU32(header + kOffPayloadLen);
  if (payload_len > Wal::kMaxPayload) {
    *err = "wal: payload length out of range";
    return ReadResult::kCorrupt;
  }
  if (payload_len != Wal::kPageImagePayload) {
    *err = "wal: page image has an unexpected length";
    return ReadResult::kCorrupt;
  }
  const Page::PageId page_id = ReadU32(header + kOffPageId);
  if (page_id == Page::kInvalidId) {
    *err = "wal: record names an invalid page";
    return ReadResult::kCorrupt;
  }

  const std::uint64_t lsn = ReadU64(header + kOffLsn);

  // Only allocate after the length has been bounded.
  std::vector<std::uint8_t> payload(payload_len);
  in.read(reinterpret_cast<char*>(payload.data()),
          static_cast<std::streamsize>(payload_len));
  got = in.gcount();
  if (got < static_cast<std::streamsize>(payload_len)) {
    *truncated = true;  // a partially written trailing payload
    return ReadResult::kEnd;
  }

  const std::uint32_t stored = ReadU32(header + kOffChecksum);
  const std::uint32_t computed =
      Crc32(header, payload.data(), payload_len);
  if (stored != computed) {
    *err = "wal: record checksum mismatch";
    return ReadResult::kCorrupt;
  }

  rec->type = type;
  rec->lsn = lsn;
  rec->page_id = page_id;
  rec->payload_len = payload_len;
  rec->payload = std::move(payload);
  return ReadResult::kRecord;
}

}  // namespace

Wal::Wal(std::fstream file, std::string path)
    : file_(std::move(file)), path_(std::move(path)), open_(true) {}

Wal::~Wal() {
  if (open_) {
    file_.flush();
    file_.close();
    open_ = false;
  }
}

Status Wal::Open(const std::string& path, std::unique_ptr<Wal>* out) {
  if (path.empty() || out == nullptr) {
    return Bad("Wal::Open: empty path or null out");
  }

  std::fstream file(path,
                    std::ios::in | std::ios::out | std::ios::binary |
                        std::ios::app);
  if (!file.is_open()) {
    // Create an empty file, then reopen for append.
    std::fstream create(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!create.is_open()) {
      return Status::Error(DbError::kIoError,
                           "cannot create wal file: " + path);
    }
    create.close();
    file.clear();
    file.open(path, std::ios::in | std::ios::out | std::ios::binary |
                        std::ios::app);
  }
  if (!file.is_open()) {
    return Status::Error(DbError::kIoError, "cannot open wal file: " + path);
  }

  std::unique_ptr<Wal> wal(new Wal(std::move(file), path));

  // Validate the existing contents so a corrupt log is rejected up front and
  // the next LSN continues strictly after the last valid record.
  std::uint64_t last_lsn = 0;
  bool truncated = false;
  Status s = wal->Scan(&last_lsn, &truncated);
  if (!s.ok()) {
    return s;
  }
  wal->next_lsn_ = last_lsn + 1;
  wal->durable_lsn_ = last_lsn;
  *out = std::move(wal);
  return Status::Ok();
}

Status Wal::Close() {
  if (!open_) {
    return Status::Ok();  // idempotent
  }
  Status s = Flush();
  file_.close();
  open_ = false;
  return s;
}

Status Wal::AppendPageImage(Page::PageId page_id, const std::uint8_t* data,
                            std::size_t len, std::uint64_t* out_lsn) {
  if (out_lsn != nullptr) {
    *out_lsn = 0;
  }
  if (!open_) {
    return BadState("Wal::AppendPageImage: log is closed");
  }
  if (data == nullptr || len != static_cast<std::size_t>(kPageImagePayload)) {
    return Bad("Wal::AppendPageImage: bad data or length");
  }
  if (page_id == Page::kInvalidId) {
    return Bad("Wal::AppendPageImage: invalid page id");
  }

  std::uint8_t header[kHeaderSize];
  std::memset(header, 0, kHeaderSize);
  header[kOffMagic + 0] = kMagic0;
  header[kOffMagic + 1] = kMagic1;
  header[kOffMagic + 2] = kMagic2;
  header[kOffMagic + 3] = kMagic3;
  WriteU16(header + kOffVersion, kVersion);
  WriteU16(header + kOffType, static_cast<std::uint16_t>(RecordType::kPageImage));
  WriteU64(header + kOffLsn, next_lsn_);
  WriteU32(header + kOffPageId, page_id);
  WriteU32(header + kOffPayloadLen, kPageImagePayload);
  const std::uint32_t crc =
      Crc32(header, data, static_cast<std::uint32_t>(len));
  WriteU32(header + kOffChecksum, crc);

  file_.write(reinterpret_cast<const char*>(header),
              static_cast<std::streamsize>(kHeaderSize));
  file_.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(len));
  if (!file_) {
    return Status::Error(DbError::kIoError,
                         "wal: append failed at lsn " +
                             std::to_string(next_lsn_));
  }
  if (out_lsn != nullptr) {
    *out_lsn = next_lsn_;
  }
  ++next_lsn_;
  return Status::Ok();
}

Status Wal::Flush() {
  if (!open_) {
    return BadState("Wal::Flush: log is closed");
  }
  file_.flush();
  if (!file_) {
    return Status::Error(DbError::kIoError, "wal: flush failed");
  }
  durable_lsn_ = next_lsn_ - 1;
  return Status::Ok();
}

Status Wal::BeforePageWrite(const Page& page) {
  if (!page.IsValid()) {
    return Bad("Wal::BeforePageWrite: invalid page");
  }
  Status s = AppendPageImage(page.id(), page.data(), Page::size(), nullptr);
  if (!s.ok()) {
    return s;
  }
  return Flush();
}

Status Wal::Scan(std::uint64_t* out_last_lsn, bool* out_truncated) const {
  if (!open_) {
    return BadState("Wal::Scan: log is closed");
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in.is_open()) {
    return Status::Error(DbError::kIoError, "wal: cannot read " + path_);
  }

  std::uint64_t last_lsn = 0;
  bool truncated = false;
  for (;;) {
    ParsedRecord rec;
    std::string err;
    bool tail = false;
    const ReadResult r = ReadRecord(in, &rec, &err, &tail);
    if (r == ReadResult::kEnd) {
      truncated = tail;
      break;
    }
    if (r == ReadResult::kCorrupt) {
      return BadState(err.c_str());
    }
    if (rec.lsn <= last_lsn) {
      return BadState("wal: log sequence numbers are not increasing");
    }
    last_lsn = rec.lsn;
  }

  if (out_last_lsn != nullptr) {
    *out_last_lsn = last_lsn;
  }
  if (out_truncated != nullptr) {
    *out_truncated = truncated;
  }
  return Status::Ok();
}

Status Wal::Recover(PageStorage* storage, RecoveryStats* stats) const {
  if (stats != nullptr) {
    *stats = RecoveryStats();
  }
  if (storage == nullptr) {
    return Bad("Wal::Recover: null storage");
  }
  if (!open_) {
    return BadState("Wal::Recover: log is closed");
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in.is_open()) {
    return Status::Error(DbError::kIoError, "wal: cannot read " + path_);
  }

  std::uint64_t last_lsn = 0;
  std::uint64_t applied = 0;
  bool truncated = false;
  for (;;) {
    ParsedRecord rec;
    std::string err;
    bool tail = false;
    const ReadResult r = ReadRecord(in, &rec, &err, &tail);
    if (r == ReadResult::kEnd) {
      truncated = tail;
      break;
    }
    if (r == ReadResult::kCorrupt) {
      return BadState(err.c_str());
    }
    if (rec.lsn <= last_lsn) {
      return BadState("wal: log sequence numbers are not increasing");
    }
    last_lsn = rec.lsn;

    if (!storage->PageExists(rec.page_id)) {
      return BadState("wal: record names an unallocated page");
    }
    Page page(rec.page_id);
    std::memcpy(page.data(), rec.payload.data(), rec.payload_len);
    Status s = storage->WritePage(page);
    if (!s.ok()) {
      return s;
    }
    ++applied;
  }

  if (stats != nullptr) {
    stats->records_applied = applied;
    stats->last_lsn = last_lsn;
    stats->truncated_tail = truncated;
  }
  return Status::Ok();
}

Status Wal::Reset() {
  if (!open_) {
    return BadState("Wal::Reset: log is closed");
  }
  file_.close();
  file_.clear();
  file_.open(path_, std::ios::in | std::ios::out | std::ios::binary |
                       std::ios::trunc);
  if (!file_.is_open()) {
    return Status::Error(DbError::kIoError, "wal: cannot truncate " + path_);
  }
  next_lsn_ = 1;
  durable_lsn_ = 0;
  return Status::Ok();
}

}  // namespace nexusdb
