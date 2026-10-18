// NexusDB — heap table over SlottedPage + BufferPool (Digvijay: Database
// Engine, R1.2).
//
// A HeapTable stores variable-length records across many fixed-size database
// pages. Each data page is an independent SlottedPage; the table keeps a
// directory that maps the table to the set of pages it owns, so record ids
// stay stable and no page-id contiguity is assumed.
//
// Ownership / layering contract
// -----------------------------
//   - HeapTable does NOT touch the database file for record I/O. All page
//     reads/writes go through BufferPool::Fetch + Handle (the pool is the only
//     mediator of page bytes), exactly as documented on BufferPool. HeapTable
//     uses PageStorage only to ALLOCATE brand-new pages (AllocatePage), which
//     is the sanctioned storage API, never a raw file handle.
//   - HeapTable does not own the storage or the pool; both must outlive it.
//   - Dirty pages are marked through the page itself (SlottedPage marks the
//     Page dirty on every successful mutation), so the BufferPool flushes them
//     on FlushAll / eviction. HeapTable never flushes by itself.
//
// On-disk page organization (two page kinds)
// ------------------------------------------
//   Data page: a plain SlottedPage (magic 'N','S','P','1'); see
//   include/db/slotted_page.h. Slot ids are stable across compaction, so a
//   record id remains valid for the life of the page.
//
//   Directory page (custom, magic 'N','H','T','1'; layout below). A table has
//   one or more directory pages forming a singly-linked chain from the root.
//   Each directory page lists the data-page ids it owns and points at the next
//   directory page. This is the minimal recoverable structure needed to find a
//   table's pages after a reopen without assuming contiguous page ids:
//
//     offset  size  field
//     0       4     magic 'N','H','T','1'
//     4       2     version (uint16) = 1
//     6       2     reserved (uint16) = 0
//     8       4     count (uint32): number of valid data-page ids here
//     12      4     next (uint32): next directory page id, or 0xFFFFFFFF (none)
//     16      ...   count * uint32 data-page ids
//
//   kDirectoryCapacity data-page ids fit in one directory page; a table with
//   more pages extends the chain. Every next link is validated (exists, not the
//   current page, not the previous page) and the walk is bounded before it is
//   followed, so a corrupt or cyclic chain is rejected with kBadState rather
//   than looped over.
//
// Record ids
// ----------
//   A RecordId is { page, slot }: the data-page id and the SlottedPage slot
//   id. It is returned only after the record has been inserted successfully.
//   Ids are validated on use: an unknown/deleted page, an out-of-range slot, a
//   tombstoned slot, or a malformed page all fail cleanly.
//
// Failure handling
// ----------------
//   - A new data page is appended to the directory only AFTER the record has
//     been inserted into it, so a failed insert never leaves a partially
//     described page in the directory and never loses existing records.
//   - Records larger than a page can hold (SlottedPage::kMaxRecordSize) are
//     rejected up front with kBufferFull, so insertion can never loop
//     allocating pages.
//   - Errors from the BufferPool (including kBufferFull when every frame is
//     pinned) are propagated unchanged.
//   - No exception escapes the public API.

#ifndef NEXUSDB_DB_HEAP_TABLE_H
#define NEXUSDB_DB_HEAP_TABLE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/slotted_page.h"

namespace nexusdb {

class HeapTable {
 public:
  // Directory page layout constants (see the file header). Exposed so the
  // format is inspectable and testable.
  static constexpr std::size_t kDirectoryHeaderSize = 16;
  static constexpr std::size_t kDirectoryIdSize = 4;
  static constexpr std::size_t kDirectoryCapacity =
      (static_cast<std::size_t>(kPageSize) - kDirectoryHeaderSize) /
      kDirectoryIdSize;
  static constexpr std::uint16_t kDirectoryVersion = 1;

  // Stable identifier for a record: the data page it lives on and its slot.
  struct RecordId {
    Page::PageId page = Page::kInvalidId;
    std::uint16_t slot = SlottedPage::kInvalidSlot;

    bool valid() const {
      return page != Page::kInvalidId && slot != SlottedPage::kInvalidSlot;
    }
    bool operator==(const RecordId& other) const {
      return page == other.page && slot == other.slot;
    }
    bool operator!=(const RecordId& other) const { return !(*this == other); }
  };

  // Record scan visitor: `id` identifies the record, `data`/`len` are its
  // bytes (valid only while the callback runs). Returning false stops the scan
  // early; the scan still succeeds.
  using ScanFn = bool (*)(const RecordId& id, const std::uint8_t* data,
                          std::size_t len, void* ctx);

  HeapTable(const HeapTable&) = delete;
  HeapTable& operator=(const HeapTable&) = delete;

  // Allocates a fresh root directory page, initializes an empty table on it,
  // and returns the table. On success `*out_root` (when non-null) receives the
  // root page id, which is what a later Open() needs. Fails with
  // kInvalidArgument on null `out`/`storage`/`pool`, or a storage error while
  // allocating/initializing the root page.
  static Status Create(PageStorage* storage, BufferPool* pool,
                       std::unique_ptr<HeapTable>* out,
                       Page::PageId* out_root);

  // Opens the table rooted at `root`. Validates the directory chain (magic,
  // version, counts, next links) before following it. Fails with
  // kInvalidArgument on null arguments or `root` == Page::kInvalidId, a storage
  // error when the root page cannot be read, or kBadState on a malformed or
  // cyclic directory chain.
  static Status Open(PageStorage* storage, BufferPool* pool,
                     Page::PageId root, std::unique_ptr<HeapTable>* out);

  // Root directory page id.
  Page::PageId root() const { return root_; }

  // Number of data pages currently owned by the table.
  std::size_t data_page_count() const { return data_pages_.size(); }

  // Inserts `len` bytes from `data` and, on success, writes the new record's
  // id to `*out`. Reuses an existing page that has room; allocates a new data
  // page only when no existing page can hold the record.
  // Errors:
  //   kInvalidArgument  null page/storage, null `data`, or len == 0
  //   kBufferFull       len > SlottedPage::kMaxRecordSize, or the BufferPool
  //                     reports no free/unpinned frame to load a needed page
  //   (pool error)      a page could not be fetched
  //   (page error)      a data page is malformed (propagated from SlottedPage)
  Status Insert(const std::uint8_t* data, std::size_t len, RecordId* out);

  // Reads the record named by `id`. On success `*out_data` points INTO the
  // pooled page (valid only until the page is evicted) and `*out_len` is its
  // length. Both out-parameters are zeroed on failure.
  // Errors:
  //   kInvalidArgument  null out-params or an invalid `id`
  //   kNotFound         `id.page` is not one of this table's data pages, or the
  //                     slot is a tombstone
  //   (pool/page error) the page could not be fetched or is malformed
  Status Get(const RecordId& id, const std::uint8_t** out_data,
             std::size_t* out_len) const;

  // Rewrites the record named by `id` (see SlottedPage::Update for the
  // in-place vs relocate rule). Errors mirror Get() plus kBufferFull when a
  // grown record cannot be placed in its page's free region.
  Status Update(const RecordId& id, const std::uint8_t* data, std::size_t len);

  // Deletes the record named by `id` (writes a tombstone). Errors mirror Get().
  Status Delete(const RecordId& id);

  // Visits every live record, page by page in directory order and slot by slot
  // within a page. Tombstones are omitted. Returning false from `fn` stops the
  // scan early. Fails with kInvalidArgument on a null callback or a pool/page
  // error while fetching/validating a data page.
  Status Scan(ScanFn fn, void* ctx) const;

 private:
  HeapTable(PageStorage* storage, BufferPool* pool, Page::PageId root)
      : storage_(storage), pool_(pool), root_(root) {}

  // Walks the directory chain from the root, validating each link, and rebuilds
  // the in-memory directory + last-page bookkeeping.
  Status LoadDirectory();

  // Appends `id` to the directory, extending the chain with a fresh directory
  // page when the last one is full.
  Status AppendDataPageId(Page::PageId id);

  // Allocates and initializes (as an empty SlottedPage) a new data page,
  // writing its header through PageStorage so a later Fetch sees it. Does NOT
  // add it to the directory; the caller does that after a successful insert.
  Status AllocateDataPage(Page::PageId* out);

  // Fetches `page` and inserts into it as a SlottedPage. Marks the page dirty
  // on success (via SlottedPage). Propagates fetch/page errors.
  Status TryInsertIntoPage(Page::PageId page, const std::uint8_t* data,
                           std::size_t len, RecordId* out);

  // True when `page` is one of this table's data pages.
  bool IsDataPage(Page::PageId page) const;

  PageStorage* storage_;
  BufferPool* pool_;
  Page::PageId root_;

  // In-memory directory cache, rebuilt by LoadDirectory and kept in sync on
  // append. The on-disk directory remains the source of truth for reopen.
  Page::PageId last_dir_ = Page::kInvalidId;
  std::uint32_t last_dir_count_ = 0;
  std::vector<Page::PageId> data_pages_;
  Page::PageId last_data_page_ = Page::kInvalidId;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_HEAP_TABLE_H
