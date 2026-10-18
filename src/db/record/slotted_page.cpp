// NexusDB — slotted-page record storage implementation (R1.1).
//
// See include/db/slotted_page.h for the on-page layout, invariants, and the
// documented update/compaction policy. All field access goes through the
// validated header; no byte is touched before the page passes Validate().

#include "db/slotted_page.h"

#include <cstring>

namespace nexusdb {
namespace {

// Magic bytes: 'N','S','P','1' ("Nexus Slotted Page, v1").
constexpr std::uint8_t kMagic0 = 0x4Eu;  // 'N'
constexpr std::uint8_t kMagic1 = 0x53u;  // 'S'
constexpr std::uint8_t kMagic2 = 0x50u;  // 'P'
constexpr std::uint8_t kMagic3 = 0x31u;  // '1'

// Header field byte offsets (see the layout table in the header).
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffSlotCount = 6;
constexpr std::size_t kOffFreeStart = 8;
constexpr std::size_t kOffFreeEnd = 10;
constexpr std::size_t kOffReserved = 12;

inline Status Bad(const char* message) {
  return Status::Error(DbError::kInvalidArgument, message);
}

}  // namespace

std::uint16_t SlottedPage::ReadU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(p[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8));
}

void SlottedPage::WriteU16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

Status SlottedPage::Init(Page* page) {
  if (page == nullptr) {
    return Bad("SlottedPage::Init: null page");
  }
  std::uint8_t* d = page->data();
  std::memset(d, 0, Page::size());
  d[kOffMagic + 0] = kMagic0;
  d[kOffMagic + 1] = kMagic1;
  d[kOffMagic + 2] = kMagic2;
  d[kOffMagic + 3] = kMagic3;
  WriteU16(d + kOffVersion, kVersion);
  WriteU16(d + kOffSlotCount, 0);
  WriteU16(d + kOffFreeStart, static_cast<std::uint16_t>(kHeaderSize));
  WriteU16(d + kOffFreeEnd, static_cast<std::uint16_t>(Page::size()));
  WriteU16(d + kOffReserved, 0);
  page->MarkDirty();
  return Status::Ok();
}

Status SlottedPage::Validate(const Page& page) {
  const std::uint8_t* d = page.data();

  if (d[kOffMagic + 0] != kMagic0 || d[kOffMagic + 1] != kMagic1 ||
      d[kOffMagic + 2] != kMagic2 || d[kOffMagic + 3] != kMagic3) {
    return Bad("slotted page: bad magic");
  }
  if (ReadU16(d + kOffVersion) != kVersion) {
    return Bad("slotted page: unsupported version");
  }

  const std::size_t slot_count = ReadU16(d + kOffSlotCount);
  // Directory must fit on the page (also bounds slot ids below kInvalidSlot).
  const std::size_t dir_end =
      kHeaderSize + slot_count * static_cast<std::size_t>(kSlotEntrySize);
  if (dir_end > Page::size()) {
    return Bad("slotted page: slot directory overflows the page");
  }

  const std::size_t free_start = ReadU16(d + kOffFreeStart);
  const std::size_t free_end = ReadU16(d + kOffFreeEnd);
  if (free_start < dir_end) {
    return Bad("slotted page: free_start overlaps the slot directory");
  }
  if (free_end < free_start) {
    return Bad("slotted page: free_end precedes free_start");
  }
  if (free_end > Page::size()) {
    return Bad("slotted page: free_end beyond the page");
  }

  // Every live slot must describe bytes inside the record region, with no
  // arithmetic overflow (offsets/lengths are promoted to std::size_t first).
  for (std::size_t i = 0; i < slot_count; ++i) {
    const std::uint8_t* e = d + kHeaderSize + i * kSlotEntrySize;
    const std::size_t offset = ReadU16(e);
    const std::size_t length = ReadU16(e + 2);
    if (length == 0) {
      continue;  // tombstone
    }
    if (offset < free_end) {
      return Bad("slotted page: live slot points into the free region");
    }
    if (offset + length > Page::size()) {
      return Bad("slotted page: live slot extends beyond the page");
    }
  }
  return Status::Ok();
}

Status SlottedPage::Validate() const {
  if (page_ == nullptr) {
    return Bad("SlottedPage: null page");
  }
  return Validate(*page_);
}

std::uint16_t SlottedPage::slot_count() const {
  return page_ != nullptr ? ReadU16(page_->data() + kOffSlotCount) : 0;
}

std::uint16_t SlottedPage::live_count() const {
  if (page_ == nullptr) {
    return 0;
  }
  const std::uint8_t* d = page_->data();
  const std::uint16_t count = ReadU16(d + kOffSlotCount);
  std::uint16_t live = 0;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint8_t* e = d + kHeaderSize +
                            static_cast<std::size_t>(i) * kSlotEntrySize;
    if (ReadU16(e + 2) != 0) {
      ++live;
    }
  }
  return live;
}

std::size_t SlottedPage::free_bytes() const {
  if (page_ == nullptr) {
    return 0;
  }
  const std::uint8_t* d = page_->data();
  const std::size_t free_start = ReadU16(d + kOffFreeStart);
  const std::size_t free_end = ReadU16(d + kOffFreeEnd);
  if (free_end < free_start || free_end > Page::size()) {
    return 0;
  }
  return free_end - free_start;
}

Status SlottedPage::Insert(const std::uint8_t* data, std::size_t len,
                           SlotId* out_slot) {
  if (out_slot != nullptr) {
    *out_slot = kInvalidSlot;
  }
  if (page_ == nullptr) {
    return Bad("SlottedPage::Insert: null page");
  }
  if (data == nullptr || len == 0) {
    return Bad("SlottedPage::Insert: null or empty record");
  }
  if (len > kMaxRecordSize) {
    return Status::Error(DbError::kBufferFull,
                         "record exceeds the maximum page record size");
  }
  Status s = Validate(*page_);
  if (!s.ok()) {
    return s;
  }

  std::uint8_t* d = page_->data();
  const std::uint16_t count = ReadU16(d + kOffSlotCount);
  const std::size_t dir_end =
      kHeaderSize + static_cast<std::size_t>(count) * kSlotEntrySize;
  const std::size_t free_start = ReadU16(d + kOffFreeStart);
  const std::size_t free_end = ReadU16(d + kOffFreeEnd);

  // A new slot entry (4 bytes) plus the record must both fit.
  const std::size_t need = kSlotEntrySize + len;
  if (free_end - free_start < need) {
    return Status::Error(DbError::kBufferFull, "page full");
  }

  // Pack the record at the bottom of the free region (top of the page).
  const std::size_t new_offset = free_end - len;
  std::memcpy(d + new_offset, data, len);

  // New slot entry at the end of the directory.
  WriteU16(d + dir_end, static_cast<std::uint16_t>(new_offset));
  WriteU16(d + dir_end + 2, static_cast<std::uint16_t>(len));

  WriteU16(d + kOffSlotCount, static_cast<std::uint16_t>(count + 1));
  WriteU16(d + kOffFreeStart,
           static_cast<std::uint16_t>(free_start + kSlotEntrySize));
  WriteU16(d + kOffFreeEnd, static_cast<std::uint16_t>(new_offset));

  page_->MarkDirty();
  if (out_slot != nullptr) {
    *out_slot = count;  // the slot id is the pre-insert directory length
  }
  return Status::Ok();
}

Status SlottedPage::Read(SlotId slot, const std::uint8_t** out_data,
                         std::size_t* out_len) const {
  if (out_data != nullptr) {
    *out_data = nullptr;
  }
  if (out_len != nullptr) {
    *out_len = 0;
  }
  if (out_data == nullptr || out_len == nullptr) {
    return Bad("SlottedPage::Read: null out-parameter");
  }
  if (page_ == nullptr) {
    return Bad("SlottedPage::Read: null page");
  }
  Status s = Validate(*page_);
  if (!s.ok()) {
    return s;
  }

  const std::uint8_t* d = page_->data();
  const std::size_t count = ReadU16(d + kOffSlotCount);
  if (static_cast<std::size_t>(slot) >= count) {
    return Bad("SlottedPage::Read: slot index out of range");
  }
  const std::uint8_t* e =
      d + kHeaderSize + static_cast<std::size_t>(slot) * kSlotEntrySize;
  const std::size_t offset = ReadU16(e);
  const std::size_t length = ReadU16(e + 2);
  if (length == 0) {
    return Status::Error(DbError::kNotFound, "slot is deleted");
  }
  *out_data = d + offset;
  *out_len = length;
  return Status::Ok();
}

Status SlottedPage::Update(SlotId slot, const std::uint8_t* data,
                           std::size_t len) {
  if (page_ == nullptr) {
    return Bad("SlottedPage::Update: null page");
  }
  if (data == nullptr || len == 0) {
    return Bad("SlottedPage::Update: null or empty record");
  }
  if (len > kMaxRecordSize) {
    return Status::Error(DbError::kBufferFull,
                         "record exceeds the maximum page record size");
  }
  Status s = Validate(*page_);
  if (!s.ok()) {
    return s;
  }

  std::uint8_t* d = page_->data();
  const std::size_t count = ReadU16(d + kOffSlotCount);
  if (static_cast<std::size_t>(slot) >= count) {
    return Bad("SlottedPage::Update: slot index out of range");
  }
  std::uint8_t* e =
      d + kHeaderSize + static_cast<std::size_t>(slot) * kSlotEntrySize;
  const std::size_t offset = ReadU16(e);
  const std::size_t old_len = ReadU16(e + 2);
  if (old_len == 0) {
    return Status::Error(DbError::kNotFound, "slot is deleted");
  }

  if (len <= old_len) {
    // In place: the slot offset is unchanged; a shorter update leaves the
    // leftover tail as fragmentation (reclaimed by Compact()).
    std::memcpy(d + offset, data, len);
    WriteU16(e + 2, static_cast<std::uint16_t>(len));
    page_->MarkDirty();
    return Status::Ok();
  }

  // Grow: relocate into the contiguous free region. Fail without touching the
  // page when there is not enough room (caller may Compact() and retry).
  const std::size_t free_start = ReadU16(d + kOffFreeStart);
  const std::size_t free_end = ReadU16(d + kOffFreeEnd);
  if (free_end - free_start < len) {
    return Status::Error(DbError::kBufferFull,
                         "record cannot grow in the free region");
  }
  const std::size_t new_offset = free_end - len;
  std::memcpy(d + new_offset, data, len);
  WriteU16(e, static_cast<std::uint16_t>(new_offset));
  WriteU16(e + 2, static_cast<std::uint16_t>(len));
  WriteU16(d + kOffFreeEnd, static_cast<std::uint16_t>(new_offset));
  page_->MarkDirty();
  return Status::Ok();
}

Status SlottedPage::Delete(SlotId slot) {
  if (page_ == nullptr) {
    return Bad("SlottedPage::Delete: null page");
  }
  Status s = Validate(*page_);
  if (!s.ok()) {
    return s;
  }

  std::uint8_t* d = page_->data();
  const std::size_t count = ReadU16(d + kOffSlotCount);
  if (static_cast<std::size_t>(slot) >= count) {
    return Bad("SlottedPage::Delete: slot index out of range");
  }
  std::uint8_t* e =
      d + kHeaderSize + static_cast<std::size_t>(slot) * kSlotEntrySize;
  if (ReadU16(e + 2) == 0) {
    return Status::Error(DbError::kNotFound, "slot is already deleted");
  }
  WriteU16(e, 0);
  WriteU16(e + 2, 0);
  page_->MarkDirty();
  return Status::Ok();
}

Status SlottedPage::Compact() {
  if (page_ == nullptr) {
    return Bad("SlottedPage::Compact: null page");
  }
  Status s = Validate(*page_);
  if (!s.ok()) {
    return s;
  }

  std::uint8_t* d = page_->data();
  const std::size_t count = ReadU16(d + kOffSlotCount);
  const std::size_t free_start = ReadU16(d + kOffFreeStart);

  // Pre-pass: total live bytes must fit above the slot directory. This guards
  // the packing loop against underflow from a corrupt (overlapping) directory
  // and makes the operation a no-op on such a page.
  std::size_t total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t length =
        ReadU16(d + kHeaderSize + i * kSlotEntrySize + 2);
    if (length == 0) {
      continue;
    }
    if (total + length > Page::size()) {
      return Bad("slotted page: live records exceed the page (corrupt)");
    }
    total += length;
  }
  if (total > Page::size() - free_start) {
    return Bad("slotted page: live records do not fit above the directory");
  }

  // Pack live records against the top of the page in slot order, staging them
  // in a page-sized scratch buffer so source reads and destination writes never
  // alias. New offsets are written back into the (unmoved) slot directory.
  std::uint8_t scratch[Page::size()];
  std::size_t dest_end = Page::size();
  for (std::size_t i = 0; i < count; ++i) {
    std::uint8_t* e = d + kHeaderSize + i * kSlotEntrySize;
    const std::size_t offset = ReadU16(e);
    const std::size_t length = ReadU16(e + 2);
    if (length == 0) {
      continue;
    }
    dest_end -= length;
    std::memcpy(scratch + dest_end, d + offset, length);
    WriteU16(e, static_cast<std::uint16_t>(dest_end));
  }

  const std::size_t region = Page::size() - dest_end;
  if (region > 0) {
    std::memcpy(d + dest_end, scratch + dest_end, region);
  }
  WriteU16(d + kOffFreeEnd, static_cast<std::uint16_t>(dest_end));
  // Zero the reclaimed free region so no stale record bytes remain observable.
  if (dest_end > free_start) {
    std::memset(d + free_start, 0, dest_end - free_start);
  }
  page_->MarkDirty();
  return Status::Ok();
}

Status SlottedPage::Scan(ScanFn fn, void* ctx) const {
  if (fn == nullptr) {
    return Bad("SlottedPage::Scan: null callback");
  }
  if (page_ == nullptr) {
    return Bad("SlottedPage::Scan: null page");
  }
  Status s = Validate(*page_);
  if (!s.ok()) {
    return s;
  }

  const std::uint8_t* d = page_->data();
  const std::size_t count = ReadU16(d + kOffSlotCount);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint8_t* e = d + kHeaderSize + i * kSlotEntrySize;
    const std::size_t offset = ReadU16(e);
    const std::size_t length = ReadU16(e + 2);
    if (length == 0) {
      continue;
    }
    if (!fn(static_cast<SlotId>(i), d + offset, length, ctx)) {
      break;  // early stop is still a successful scan
    }
  }
  return Status::Ok();
}

}  // namespace nexusdb
