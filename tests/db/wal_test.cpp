// NexusDB — R2 write-ahead log test.
//
// Deterministic tests of the Wal record format, LSN bookkeeping, checksums,
// corruption rejection, truncated-tail handling, replay and reset, against real
// files in the OS temp directory (removed before/after). Recovery is exercised
// by replaying real page images into a real PageStorage and comparing bytes.
//
// Same plain PASS/FAIL style as the other DB tests: exit 0 on success, 1 on any
// failure. No timing assumptions.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/wal.h"

namespace {

using nexusdb::BufferPool;
using nexusdb::DbError;
using nexusdb::Page;
using nexusdb::PageStorage;
using nexusdb::Status;
using nexusdb::Wal;

int g_failures = 0;

#define CHECK(desc, expr)                     \
  do {                                        \
    if (expr) {                               \
      printf("PASS: %s\n", desc);             \
    } else {                                  \
      printf("FAIL: %s\n", desc);             \
      ++g_failures;                           \
    }                                         \
  } while (0)

constexpr std::size_t kRec = Wal::kHeaderSize + Wal::kPageImagePayload;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffType = 6;
constexpr std::size_t kOffLsn = 8;
constexpr std::size_t kOffPageId = 16;
constexpr std::size_t kOffPayloadLen = 20;
constexpr std::size_t kOffChecksum = 28;

std::string TestPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void RemoveFile(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void FillPage(std::uint8_t* p, std::uint8_t seed, Page::PageId id) {
  for (std::size_t i = 0; i < Page::size(); ++i) {
    p[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
  }
  p[0] = static_cast<std::uint8_t>(id & 0xFFu);
  p[1] = static_cast<std::uint8_t>((id >> 8) & 0xFFu);
}

bool PageMatches(const std::uint8_t* p, std::uint8_t seed, Page::PageId id) {
  if (p[0] != static_cast<std::uint8_t>(id & 0xFFu) ||
      p[1] != static_cast<std::uint8_t>((id >> 8) & 0xFFu)) {
    return false;
  }
  for (std::size_t i = 2; i < Page::size(); ++i) {
    if (p[i] != static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i))) {
      return false;
    }
  }
  return true;
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

// Test-local CRC-32 (IEEE) so a header field can be patched while keeping the
// checksum valid, isolating validation of that individual field.
std::uint32_t Crc32(const std::uint8_t* header, const std::uint8_t* payload,
                    std::uint32_t payload_len) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < kOffChecksum; ++i) {
    crc ^= header[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  for (std::uint32_t i = 0; i < payload_len; ++i) {
    crc ^= payload[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// Reads the whole file.
std::vector<std::uint8_t> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

void WriteAll(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// Recomputes record `rec_start`'s checksum in `bytes` (used after patching a
// header field so only that field is under test).
void FixChecksum(std::vector<std::uint8_t>& bytes, std::size_t rec_start) {
  const std::uint8_t* header = bytes.data() + rec_start;
  const std::uint8_t* payload = header + Wal::kHeaderSize;
  const std::uint32_t crc = Crc32(header, payload, Wal::kPageImagePayload);
  WriteU32(bytes.data() + rec_start + kOffChecksum, crc);
}

std::unique_ptr<PageStorage> OpenStorage(const std::string& path) {
  std::unique_ptr<PageStorage> storage;
  if (!PageStorage::Open(path, &storage).ok()) {
    return nullptr;
  }
  return storage;
}

std::unique_ptr<Wal> OpenWal(const std::string& path) {
  std::unique_ptr<Wal> wal;
  if (!Wal::Open(path, &wal).ok()) {
    return nullptr;
  }
  return wal;
}

bool AppendImage(const std::string& wal_path, Page::PageId id, std::uint8_t seed,
                 std::unique_ptr<Wal>* wal_out = nullptr) {
  std::unique_ptr<Wal> wal = OpenWal(wal_path);
  if (wal == nullptr) {
    return false;
  }
  std::vector<std::uint8_t> buf(Page::size());
  FillPage(buf.data(), seed, id);
  if (!wal->AppendPageImage(id, buf.data(), buf.size()).ok()) {
    return false;
  }
  if (!wal->Flush().ok()) {
    return false;
  }
  if (wal_out != nullptr) {
    *wal_out = std::move(wal);
  }
  return true;
}

bool ReadPageBytes(PageStorage* storage, Page::PageId id,
                   std::vector<std::uint8_t>* out) {
  Page page;
  if (!storage->ReadPage(id, &page).ok()) {
    return false;
  }
  out->assign(page.data(), page.data() + Page::size());
  return true;
}

}  // namespace

int main() {
  printf("=== NexusDB R2: write-ahead log test ===\n\n");

  // ---- 1. Create / open an empty log ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_empty.wal");
    RemoveFile(wal_path);
    std::unique_ptr<Wal> wal;
    CHECK("Open creates a log",
          Wal::Open(wal_path, &wal).ok() && wal != nullptr);
    CHECK("a fresh log is open", wal->IsOpen());
    CHECK("a fresh log starts at lsn 1", wal->next_lsn() == 1);
    CHECK("a fresh log has no durable records", wal->durable_lsn() == 0);
    CHECK("Open with an empty path fails",
          Wal::Open("", &wal).code == DbError::kInvalidArgument);
    CHECK("Open with null out fails",
          Wal::Open(wal_path, nullptr).code == DbError::kInvalidArgument);
    CHECK("Close succeeds", wal->Close().ok());
    CHECK("a closed log rejects Append",
          wal->AppendPageImage(1, nullptr, Page::size()).code ==
              DbError::kBadState);
    RemoveFile(wal_path);
  }

  // ---- 2. Append, LSN growth, durability ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_append.wal");
    RemoveFile(wal_path);
    std::unique_ptr<Wal> wal;
    Wal::Open(wal_path, &wal);

    std::vector<std::uint8_t> buf(Page::size());
    FillPage(buf.data(), 1, 5);
    std::uint64_t lsn0 = 0;
    CHECK("first append returns lsn 1",
          wal->AppendPageImage(5, buf.data(), buf.size(), &lsn0).ok() &&
              lsn0 == 1);
    CHECK("lsn advances to 2", wal->next_lsn() == 2);
    CHECK("nothing durable before Flush", wal->durable_lsn() == 0);
    CHECK("Flush succeeds", wal->Flush().ok());
    CHECK("durable lsn is 1 after Flush", wal->durable_lsn() == 1);

    FillPage(buf.data(), 2, 6);
    std::uint64_t lsn1 = 0;
    CHECK("second append gets lsn 2",
          wal->AppendPageImage(6, buf.data(), buf.size(), &lsn1).ok() &&
              lsn1 == 2);
    CHECK("Flush after the second append", wal->Flush().ok());
    CHECK("file holds two records", ReadAll(wal_path).size() == 2 * kRec);

    CHECK("append of null data fails",
          wal->AppendPageImage(6, nullptr, Page::size()).code ==
              DbError::kInvalidArgument);
    CHECK("append of a short page fails",
          wal->AppendPageImage(6, buf.data(), 10).code ==
              DbError::kInvalidArgument);
    CHECK("append with an invalid page id fails",
          wal->AppendPageImage(Page::kInvalidId, buf.data(), buf.size()).code ==
              DbError::kInvalidArgument);
    wal->Close();
    RemoveFile(wal_path);
  }

  // ---- 3. Reopen continues the LSN sequence ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_reopen.wal");
    RemoveFile(wal_path);
    AppendImage(wal_path, 3, 7);
    AppendImage(wal_path, 4, 8);

    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    CHECK("reopen scans the existing records", wal != nullptr);
    CHECK("next lsn continues after the last record", wal->next_lsn() == 3);
    CHECK("durable lsn reflects the existing records", wal->durable_lsn() == 2);
    wal->Close();
    RemoveFile(wal_path);
  }

  // ---- 4. Recover replays page images ----
  {
    const std::string db_path = TestPath("nexusdb_wal_recover.db");
    const std::string wal_path = TestPath("nexusdb_wal_recover.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);

    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1, p2;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);
    storage->AllocatePage(&p2);

    AppendImage(wal_path, 1, 0x11);
    AppendImage(wal_path, 2, 0x22);

    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    Wal::RecoveryStats stats;
    CHECK("recover succeeds", wal->Recover(storage.get(), &stats).ok());
    CHECK("recover applied two records", stats.records_applied == 2);
    CHECK("recover reports the last lsn", stats.last_lsn == 2);
    CHECK("recover saw no truncated tail", !stats.truncated_tail);

    std::vector<std::uint8_t> got;
    CHECK("recovered page 1 has the image bytes",
          ReadPageBytes(storage.get(), 1, &got) &&
              PageMatches(got.data(), 0x11, 1));
    CHECK("recovered page 2 has the image bytes",
          ReadPageBytes(storage.get(), 2, &got) &&
              PageMatches(got.data(), 0x22, 2));
    CHECK("unlogged page 0 is untouched",
          ReadPageBytes(storage.get(), 0, &got) &&
              !PageMatches(got.data(), 0x11, 1));

    // Idempotent: a second recovery yields the same bytes.
    CHECK("second recover succeeds", wal->Recover(storage.get()).ok());
    CHECK("second recover leaves page 1 identical",
          ReadPageBytes(storage.get(), 1, &got) &&
              PageMatches(got.data(), 0x11, 1));

    CHECK("recover rejects null storage",
          wal->Recover(nullptr).code == DbError::kInvalidArgument);
    wal->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 5. A later image for the same page wins ----
  {
    const std::string db_path = TestPath("nexusdb_wal_last.db");
    const std::string wal_path = TestPath("nexusdb_wal_last.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);

    AppendImage(wal_path, 1, 0x33);
    AppendImage(wal_path, 1, 0x44);  // same page, newer LSN

    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    wal->Recover(storage.get());
    std::vector<std::uint8_t> got;
    CHECK("the newest image for a page wins",
          ReadPageBytes(storage.get(), 1, &got) &&
              PageMatches(got.data(), 0x44, 1));
    wal->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 6. Checksum mismatch is corruption ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_crc.wal");
    RemoveFile(wal_path);
    AppendImage(wal_path, 1, 0x55);
    std::vector<std::uint8_t> bytes = ReadAll(wal_path);
    bytes[Wal::kHeaderSize + 10] ^= 0xFFu;  // flip a payload byte
    WriteAll(wal_path, bytes);
    std::unique_ptr<Wal> wal;
    Status s = Wal::Open(wal_path, &wal);
    CHECK("checksum mismatch rejected as kBadState",
          !s.ok() && s.code == DbError::kBadState);
    RemoveFile(wal_path);
  }

  // ---- 7. Corruption in the middle of the log is rejected ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_mid.wal");
    RemoveFile(wal_path);
    AppendImage(wal_path, 1, 0x01);
    AppendImage(wal_path, 2, 0x02);
    AppendImage(wal_path, 3, 0x03);
    std::vector<std::uint8_t> bytes = ReadAll(wal_path);
    bytes[1 * kRec + Wal::kHeaderSize + 7] ^= 0xFFu;  // corrupt middle record
    WriteAll(wal_path, bytes);
    std::unique_ptr<Wal> wal;
    Status s = Wal::Open(wal_path, &wal);
    CHECK("mid-log corruption rejected as kBadState",
          !s.ok() && s.code == DbError::kBadState);
    RemoveFile(wal_path);
  }

  // ---- 8. Truncated trailing record is tolerated, not treated as corruption ----
  {
    const std::string db_path = TestPath("nexusdb_wal_trunc.db");
    const std::string wal_path = TestPath("nexusdb_wal_trunc.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0, p1, p2;
    storage->AllocatePage(&p0);
    storage->AllocatePage(&p1);
    storage->AllocatePage(&p2);

    AppendImage(wal_path, 1, 0x0a);
    AppendImage(wal_path, 2, 0x0b);
    AppendImage(wal_path, 2, 0x0c);  // will be truncated

    // Keep two full records plus a partial third (header + 100 payload bytes).
    std::error_code ec;
    std::filesystem::resize_file(wal_path, 2 * kRec + Wal::kHeaderSize + 100, ec);

    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    CHECK("Open tolerates a truncated trailing record", wal != nullptr);
    CHECK("Open recovered the two complete records", wal != nullptr &&
                                                          wal->next_lsn() == 3);
    Wal::RecoveryStats stats;
    CHECK("recover succeeds past the tail",
          wal->Recover(storage.get(), &stats).ok());
    CHECK("only the complete records were applied", stats.records_applied == 2);
    CHECK("the truncated tail is reported", stats.truncated_tail);
    std::vector<std::uint8_t> got;
    CHECK("page 2 keeps the last complete image (0x0b)",
          ReadPageBytes(storage.get(), 2, &got) &&
              PageMatches(got.data(), 0x0b, 2));
    wal->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 9. Field-level validation (each with a valid checksum) ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_fields.wal");
    RemoveFile(wal_path);
    AppendImage(wal_path, 1, 0x66);
    const std::vector<std::uint8_t> good = ReadAll(wal_path);

    auto reject = [&](std::vector<std::uint8_t> bytes, const char* desc) {
      WriteAll(wal_path, bytes);
      std::unique_ptr<Wal> wal;
      Status s = Wal::Open(wal_path, &wal);
      CHECK(desc, !s.ok() && s.code == DbError::kBadState);
    };

    {
      std::vector<std::uint8_t> b = good;
      b[0] = 0x00;  // bad magic
      reject(std::move(b), "bad magic rejected");
    }
    {
      std::vector<std::uint8_t> b = good;
      b[kOffVersion] = 9;  // bad version
      reject(std::move(b), "bad version rejected");
    }
    {
      std::vector<std::uint8_t> b = good;
      b[kOffType] = 77;  // unknown type
      reject(std::move(b), "unknown record type rejected");
    }
    {
      std::vector<std::uint8_t> b = good;
      WriteU32(b.data() + kOffPayloadLen, Wal::kMaxPayload + 1);
      reject(std::move(b), "out-of-range payload length rejected");
    }
    {
      std::vector<std::uint8_t> b = good;
      WriteU32(b.data() + kOffPageId, Page::kInvalidId);
      FixChecksum(b, 0);
      reject(std::move(b), "invalid page id rejected");
    }
    RemoveFile(wal_path);
  }

  // ---- 10. Non-monotonic LSN is rejected ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_lsn.wal");
    RemoveFile(wal_path);
    AppendImage(wal_path, 1, 0x77);
    AppendImage(wal_path, 2, 0x78);
    std::vector<std::uint8_t> bytes = ReadAll(wal_path);
    WriteU64(bytes.data() + 1 * kRec + kOffLsn, 1);  // duplicate lsn 1
    FixChecksum(bytes, 1 * kRec);  // only the LSN is under test
    WriteAll(wal_path, bytes);
    std::unique_ptr<Wal> wal;
    Status s = Wal::Open(wal_path, &wal);
    CHECK("non-increasing LSN rejected as kBadState",
          !s.ok() && s.code == DbError::kBadState);
    RemoveFile(wal_path);
  }

  // ---- 11. Recover rejects an image for an unallocated page ----
  {
    const std::string db_path = TestPath("nexusdb_wal_unalloc.db");
    const std::string wal_path = TestPath("nexusdb_wal_unalloc.wal");
    RemoveFile(db_path);
    RemoveFile(wal_path);
    std::unique_ptr<PageStorage> storage = OpenStorage(db_path);
    Page p0;  // only page 0 exists
    storage->AllocatePage(&p0);
    AppendImage(wal_path, 9, 0x99);  // page 9 is not allocated
    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    Status s = wal->Recover(storage.get());
    CHECK("recover rejects an unallocated target page as kBadState",
          !s.ok() && s.code == DbError::kBadState);
    wal->Close();
    RemoveFile(db_path);
    RemoveFile(wal_path);
  }

  // ---- 12. Reset truncates and restarts the LSN sequence ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_reset.wal");
    RemoveFile(wal_path);
    AppendImage(wal_path, 1, 0xab);
    AppendImage(wal_path, 2, 0xac);
    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    CHECK("reset succeeds", wal->Reset().ok());
    CHECK("reset empties the file", ReadAll(wal_path).empty());
    CHECK("reset restarts the lsn", wal->next_lsn() == 1);
    CHECK("reset clears the durable lsn", wal->durable_lsn() == 0);
    std::vector<std::uint8_t> buf(Page::size());
    FillPage(buf.data(), 1, 3);
    std::uint64_t lsn = 0;
    CHECK("append after reset gets lsn 1",
          wal->AppendPageImage(3, buf.data(), buf.size(), &lsn).ok() &&
              lsn == 1);
    wal->Close();
    RemoveFile(wal_path);
  }

  // ---- 13. BeforePageWrite appends and flushes atomically ----
  {
    const std::string wal_path = TestPath("nexusdb_wal_sink.wal");
    RemoveFile(wal_path);
    std::unique_ptr<Wal> wal = OpenWal(wal_path);
    Page page(7);
    FillPage(page.data(), 0x5a, 7);
    page.MarkDirty();
    CHECK("BeforePageWrite succeeds", wal->BeforePageWrite(page).ok());
    CHECK("BeforePageWrite advanced and flushed the lsn",
          wal->next_lsn() == 2 && wal->durable_lsn() == 1);
    CHECK("BeforePageWrite wrote one record",
          ReadAll(wal_path).size() == kRec);
    wal->Close();
    RemoveFile(wal_path);
  }

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
