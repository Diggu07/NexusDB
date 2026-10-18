// NexusDB — persistent catalog of tables (Digvijay: Database Engine, R1.3).
//
// The catalog is the small, persistent mapping the database needs to find its
// tables after a reopen: a table NAME -> HeapTable root page id. It is not a
// schema/type system, an index, or a query engine; it only names heap tables.
//
// Layering / ownership
// --------------------
//   - The catalog reads and writes page bytes ONLY through BufferPool::Fetch
//     and Handle, exactly as HeapTable does; it never touches the database
//     file. It uses PageStorage only to ALLOCATE pages (AllocatePage), which is
//     the sanctioned storage API.
//   - The catalog does not own the storage or the pool; both must outlive it.
//   - Dirty pages are marked through the page itself (catalog mutations call
//     Page::MarkDirty on the pooled page), so the BufferPool flushes them on
//     FlushAll/eviction. The catalog never flushes by itself.
//
// On-disk format (catalog page, magic 'N','C','A','1')
// --------------------------------------------------
//   A catalog is one or more fixed-layout "catalog pages" forming a
//   singly-linked chain from the root. Each page holds a fixed number of
//   fixed-size entries (name + table root page id):
//
//     offset  size  field
//     0       4     magic 'N','C','A','1'
//     4       2     version (uint16) = 1
//     6       2     reserved (uint16) = 0
//     8       4     count (uint32): number of valid entries on this page
//     12      4     next (uint32): next catalog page id, or 0xFFFFFFFF (none)
//     16      ...   count entries, each kEntrySize (36) bytes:
//                      name[32]   NUL-padded table name (ASCII)
//                      root u32   HeapTable root page id
//
//   kCapacity (113) entries fit on one page; a catalog with more tables
//   extends the chain. Every next link is validated (exists, not the current or
//   previous page) and the walk is bounded before it is followed, so a corrupt
//   or cyclic chain is rejected with kBadState rather than looped over.
//
//   Page ids are never assumed contiguous: each table root is an explicit id
//   recorded in the entry.
//
// Table names
// -----------
//   1..31 characters from [A-Za-z0-9_]. Names are unique across the whole
//   catalog; a duplicate on disk is treated as corruption (kBadState) and a
//   duplicate CreateTable is rejected (kInvalidArgument).
//
// Validation
// ----------
//   On Open the whole chain is validated: magic/version/count, entry name
//   syntax, no duplicate names, each table root page exists, and each root
//   actually opens as a HeapTable (which validates its own directory chain).
//   A malformed catalog therefore fails Open with kBadState/kInvalidArgument
//   instead of silently succeeding.
//
// Errors follow the project convention: every fallible call returns Status; no
// exception escapes the public API.

#ifndef NEXUSDB_DB_CATALOG_H
#define NEXUSDB_DB_CATALOG_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/heap_table.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace nexusdb {

class Catalog {
 public:
  // On-disk layout constants (documented above). Exposed so the format is
  // inspectable and testable.
  static constexpr std::size_t kHeaderSize = 16;
  static constexpr std::size_t kNameFieldSize = 32;
  static constexpr std::size_t kEntrySize = kNameFieldSize + 4;
  static constexpr std::size_t kCapacity =
      (static_cast<std::size_t>(kPageSize) - kHeaderSize) / kEntrySize;
  static constexpr std::size_t kMaxTableNameLength = kNameFieldSize - 1;
  static constexpr std::uint16_t kVersion = 1;

  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;

  // True when `name` is a syntactically valid table name (1..31 chars, each
  // from [A-Za-z0-9_]). Pure; never fails.
  static bool IsValidTableName(const std::string& name);

  // Allocates and initializes a fresh, empty catalog root page and returns an
  // open catalog. On success `*out_root` (when non-null) receives the root page
  // id needed by a later Open(). Fails with kInvalidArgument on null arguments.
  static Status Create(PageStorage* storage, BufferPool* pool,
                       std::unique_ptr<Catalog>* out, Page::PageId* out_root);

  // Opens the catalog rooted at `root`, fully validating the chain. Fails with
  // kInvalidArgument on null arguments or `root` == Page::kInvalidId, a storage
  // error when the root cannot be read, or kBadState/kInvalidArgument on a
  // malformed, cyclic, or inconsistent catalog.
  static Status Open(PageStorage* storage, BufferPool* pool, Page::PageId root,
                     std::unique_ptr<Catalog>* out);

  // Root catalog page id.
  Page::PageId root() const { return root_; }

  // Number of tables currently recorded.
  std::size_t table_count() const { return entries_.size(); }

  // True when a table named `name` exists.
  bool Exists(const std::string& name) const;

  // Writes the table root page id of `name` to `*out_root`.
  // Errors: kInvalidArgument (null out or invalid name), kNotFound (no table).
  Status FindTable(const std::string& name, Page::PageId* out_root) const;

  // Opens the named table as a HeapTable.
  // Errors: kInvalidArgument (invalid name), kNotFound (no table), or a
  // HeapTable::Open error for a corrupt table.
  Status OpenTable(const std::string& name, std::unique_ptr<HeapTable>* out);

  // Creates a new named table (allocating a fresh HeapTable root) and records
  // it. `*out_table_root` (when non-null) receives the new table root id.
  // Errors: kInvalidArgument (invalid name or name already exists), or a
  // storage/pool error while creating or recording the table.
  Status CreateTable(const std::string& name, Page::PageId* out_table_root);

  // Enumerates table names in catalog order. Errors: kInvalidArgument (null
  // out).
  Status ListTables(std::vector<std::string>* out) const;

 private:
  struct Entry {
    std::string name;
    Page::PageId root = Page::kInvalidId;
  };

  Catalog(PageStorage* storage, BufferPool* pool, Page::PageId root)
      : storage_(storage), pool_(pool), root_(root) {}

  // Walks and validates the whole chain, rebuilding `entries_` and the
  // last-page bookkeeping.
  Status Load();

  // Records a new (name, root) entry, extending the chain with a fresh catalog
  // page when the last one is full.
  Status AppendEntry(const std::string& name, Page::PageId root);

  PageStorage* storage_;
  BufferPool* pool_;
  Page::PageId root_;

  Page::PageId last_page_ = Page::kInvalidId;
  std::uint32_t last_page_count_ = 0;
  std::vector<Entry> entries_;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_CATALOG_H
