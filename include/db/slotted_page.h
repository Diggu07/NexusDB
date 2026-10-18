// NexusDB — slotted-page record storage (Digvijay: Database Engine, R1.1).
//
// A SlottedPage is a thin, self-validating view over one fixed-size Page. It
// packs variable-length records into the page's kPageSize-byte buffer and keeps
// a slot directory that maps a stable slot id to each record's offset/length.
// This is the storage primitive the record/CRUD layer (R1) will be built on;
// it knows nothing about schemas, SQL, tables, WAL, or the Buffer Pool.
//
// On-page layout (all multi-byte fields little-endian)
// ----------------------------------------------------
//   offset  size  field
//   ------  ----  ------------------------------------------------------------
//   0       4     magic  = 'N','S','P','1'  (0x4E 0x53 0x50 0x31)
//   4       2     version (uint16) = 1
//   6       2     slot_count (uint16): number of slot directory entries,
//                  INCLUDING tombstones
//   8       2     free_start (uint16): first byte of the free region; it is
//                  the end of the slot directory (records grow downward)
//   10      2     free_end  (uint16): one past ... i.e. the LOWEST byte address
//                  occupied by a live record; the record region is
//                  [free_end, kPageSize) and the free region is
//                  [free_start, free_end)
//   12      2     reserved (uint16) = 0 (alignment / future use)
//   14      2     unused padding (0)
//   16      ...   slot directory: slot_count entries of
//                  { uint16 offset; uint16 length }
//   ...           slot directory ends at free_start
//   free_start .. free_end    free region (unallocated)
//   free_end  .. kPageSize     record region (live records, packed at top)
//
// Invariants maintained by every successful mutation and required by Validate:
//   - magic and version are correct;
//   - free_start >= kHeaderSize + slot_count * kSlotEntrySize;
//   - free_start <= free_end <= kPageSize;
//   - every live slot (length > 0) has offset >= free_end and
//     offset + length <= kPageSize;
//   - every tombstone has length == 0.
//
// Slots and tombstones
// --------------------
//   - A slot id is the index of a slot in the directory; it is assigned by
//     Insert and NEVER changes for the life of the page, including across
//     Compact(). This keeps record ids stable, which the CRUD layer relies on.
//   - Deleting a slot writes a tombstone (length = 0). The record bytes are not
//     reclaimed until Compact(); a tombstoned slot is never returned by Read,
//     Update, or Scan (those report kNotFound).
//   - Zero-length records cannot be inserted, so length == 0 unambiguously
//     means "deleted".
//
// Update policy (documented so callers can reason about space):
//   - Update with a length <= the current length rewrites the record IN PLACE
//     (the slot offset does not change); a shorter update leaves the leftover
//     bytes as internal fragmentation to be reclaimed by Compact().
//   - Update with a longer length RELOCATES the record to the top of the free
//     region if `length` contiguous free bytes are available; the old bytes
//     become fragmentation. It does NOT compact automatically: if there is not
//     enough contiguous free space the call fails with kBufferFull and the page
//     is left unchanged (callers may Compact() and retry).
//
// Safety
// ------
//   - Every offset, length, slot index, and arithmetic operation is validated
//     before any page byte is accessed; malformed headers/directories yield
//     kInvalidArgument and never cause an out-of-bounds access.
//   - Every fallible call returns Status; no exception escapes this API.
//   - Mutating calls mark the Page dirty (Page::MarkDirty); read/scan do not.
//     Flushing dirty pages is the Buffer Pool's job, exactly as documented on
//     Page/PageStorage.

#ifndef NEXUSDB_DB_SLOTTED_PAGE_H
#define NEXUSDB_DB_SLOTTED_PAGE_H

#include <cstddef>
#include <cstdint>

#include "common/config.h"
#include "common/error.h"
#include "db/page.h"

namespace nexusdb {

class SlottedPage {
 public:
  // Slot directory index. kInvalidSlot is never a valid slot id (the maximum
  // representable slot count keeps ids strictly below it).
  using SlotId = std::uint16_t;
  static constexpr SlotId kInvalidSlot = 0xFFFFu;

  // On-page layout constants (see the file header for the full layout).
  static constexpr std::size_t kHeaderSize = 16;
  static constexpr std::size_t kSlotEntrySize = 4;
  static constexpr std::uint16_t kVersion = 1;

  // Largest record that can EVER be stored (a page holding only that record
  // still needs one 4-byte slot directory entry).
  static constexpr std::size_t kMaxRecordSize =
      static_cast<std::size_t>(kPageSize) - kHeaderSize - kSlotEntrySize;

  // Initializes `page` as an empty slotted page (zeroed data, slot_count 0,
  // empty free region, magic/version written) and marks it dirty. Fails with
  // kInvalidArgument when `page` is null.
  static Status Init(Page* page);

  // Validates that `page` holds a well-formed slotted page. Never modifies the
  // page. Fails with kInvalidArgument with a descriptive message when the
  // magic/version is wrong or any layout invariant is violated.
  static Status Validate(const Page& page);

  // Views `page` as a slotted page. `page` must outlive this object and must
  // have been Init()ed or previously validated. A null `page` makes every
  // operation fail with kInvalidArgument.
  explicit SlottedPage(Page* page) : page_(page) {}

  // Re-validates the underlying page (same rules as the static form).
  Status Validate() const;

  // Number of slot directory entries, including tombstones. Returns 0 when the
  // page is null or invalid. Precondition: the page was Init()ed/validated.
  std::uint16_t slot_count() const;

  // Number of live (non-tombstoned) records. Returns 0 when the page is null or
  // invalid.
  std::uint16_t live_count() const;

  // Bytes in the contiguous free region [free_start, free_end). This excludes
  // fragmentation left by deletes/shrinks until Compact() consolidates it.
  // Returns 0 when the page is null or invalid.
  std::size_t free_bytes() const;

  // Inserts `len` bytes from `data` as a new record and writes its stable slot
  // id to `*out_slot` on success. Marks the page dirty.
  // Errors:
  //   kInvalidArgument  null page, null `data`, or len == 0
  //   kBufferFull       len > kMaxRecordSize, or the page cannot fit a new slot
  //                     entry plus `len` bytes of record
  //   (validation error) the underlying page is malformed
  Status Insert(const std::uint8_t* data, std::size_t len, SlotId* out_slot);

  // Reads the live record in `slot`. On success `*out_data` points INTO the
  // page buffer (valid only while the page is unchanged) and `*out_len` is its
  // length. Both out-parameters are zeroed on failure.
  // Errors:
  //   kInvalidArgument  null out-params, or `slot` >= slot_count
  //   kNotFound         `slot` names a deleted (tombstoned) record
  //   (validation error) the underlying page is malformed
  Status Read(SlotId slot, const std::uint8_t** out_data,
              std::size_t* out_len) const;

  // Rewrites the live record in `slot`. See "Update policy" above for the
  // in-place vs relocate behaviour. Marks the page dirty on success; a failed
  // update leaves the page byte-for-byte unchanged.
  // Errors:
  //   kInvalidArgument  null page, null `data`, or len == 0
  //   kInvalidArgument  `slot` >= slot_count
  //   kNotFound         `slot` names a deleted record
  //   kBufferFull       len > kMaxRecordSize, or a grow cannot be placed in the
  //                     contiguous free region (Compact() and retry)
  //   (validation error) the underlying page is malformed
  Status Update(SlotId slot, const std::uint8_t* data, std::size_t len);

  // Deletes the live record in `slot` by writing a tombstone. Marks the page
  // dirty on success.
  // Errors:
  //   kInvalidArgument  null page, or `slot` >= slot_count
  //   kNotFound         `slot` is already a tombstone
  //   (validation error) the underlying page is malformed
  Status Delete(SlotId slot);

  // Repacks all live records against the top of the page, reclaiming
  // fragmentation from deletes and shrinks. Slot ids and slot ordering are
  // preserved exactly; only record offsets change. Tombstones stay tombstones.
  // Marks the page dirty. Errors: kInvalidArgument when the page is null,
  // malformed, or its live records cannot be packed (overlapping/corrupt
  // directory); the page is left unchanged on such an error.
  Status Compact();

  // Visits every live record in ascending slot order, invoking `fn(slot, data,
  // len, ctx)`. `data` points into the page buffer. Returning false from `fn`
  // stops the scan early; the scan then still succeeds. Errors:
  //   kInvalidArgument  null page, null `fn`, or a malformed page
  using ScanFn = bool (*)(SlotId slot, const std::uint8_t* data,
                          std::size_t len, void* ctx);
  Status Scan(ScanFn fn, void* ctx) const;

 private:
  // Raw header/directory reads. Callers must have validated first.
  static std::uint16_t ReadU16(const std::uint8_t* p);
  static void WriteU16(std::uint8_t* p, std::uint16_t v);

  Page* page_;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_SLOTTED_PAGE_H
