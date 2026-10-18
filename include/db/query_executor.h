// NexusDB — minimal query / table-access executor (Digvijay: Database Engine,
// R1.3).
//
// The executor is the smallest useful "query" surface over the storage stack.
// It composes a persistent Catalog (name -> HeapTable root) with the HeapTable
// record API; it contains NO storage logic of its own and never touches page
// bytes directly. It is deliberately NOT a SQL parser, a server, a CLI, a
// join/index/transaction engine, or a CLI facade.
//
// Table-scoped record ids
// -----------------------
//   A HeapTable::RecordId ({ page, slot }) is only meaningful WITHIN one table:
//   two tables can both hold a record at { page 5, slot 0 }, and a page id from
//   one table will not resolve in another. This API therefore requires callers
//   to pass the table NAME alongside the RecordId for every read/update/delete,
//   and it resolves the id against that table only.
//
// Layering / ownership
// --------------------
//   - The executor owns a Catalog, which owns the name->root index; the caller
//     owns the PageStorage and BufferPool, which must outlive the executor.
//   - Each operation opens the target HeapTable through the catalog for the
//     duration of that call, so no per-table state is cached here and the
//     on-disk catalog/directory remains the single source of truth.
//   - Durability follows the underlying layers: mutations mark pooled pages
//     dirty and are made durable by BufferPool::FlushAll (or eviction). The
//     executor never flushes by itself.
//   - Get()'s returned byte pointer points into the pooled page, exactly as
//     HeapTable::Get documents: it is valid only until that page is evicted.
//
// Errors follow the project convention (Status/DbError); no exception escapes
// the public API.

#ifndef NEXUSDB_DB_QUERY_EXECUTOR_H
#define NEXUSDB_DB_QUERY_EXECUTOR_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/error.h"
#include "db/catalog.h"
#include "db/heap_table.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace nexusdb {

class QueryExecutor {
 public:
  // Record identity, reused from HeapTable. Only unique within a table; see
  // the note above.
  using RecordId = HeapTable::RecordId;

  // Scan visitor, reused from HeapTable so early termination behaves
  // identically (return false to stop). `data`/`len` are valid only while the
  // callback runs.
  using ScanFn = HeapTable::ScanFn;

  QueryExecutor(const QueryExecutor&) = delete;
  QueryExecutor& operator=(const QueryExecutor&) = delete;

  // Creates a fresh, empty executor backed by a new catalog. On success
  // `*out_catalog_root` (when non-null) receives the catalog root page id used
  // to reopen later. Fails with kInvalidArgument on null arguments.
  static Status Create(PageStorage* storage, BufferPool* pool,
                       std::unique_ptr<QueryExecutor>* out,
                       Page::PageId* out_catalog_root);

  // Opens an executor over the catalog rooted at `catalog_root`.
  // Errors: kInvalidArgument (null args / invalid root) or a Catalog::Open
  // error for a missing/malformed catalog.
  static Status Open(PageStorage* storage, BufferPool* pool,
                     Page::PageId catalog_root,
                     std::unique_ptr<QueryExecutor>* out);

  // Catalog root page id (for reopening).
  Page::PageId catalog_root() const { return catalog_->root(); }

  // Number of tables in the catalog.
  std::size_t table_count() const { return catalog_->table_count(); }

  // True when a table named `name` exists.
  bool TableExists(const std::string& name) const;

  // Enumerates table names in catalog order. Errors: kInvalidArgument (null).
  Status ListTables(std::vector<std::string>* out) const;

  // Creates a named table. Errors: kInvalidArgument (invalid name or duplicate)
  // or a storage/pool error from the catalog.
  Status CreateTable(const std::string& name);

  // Inserts `len` bytes into table `table`, writing the new RecordId to `*out`
  // (when non-null). Errors: kInvalidArgument (invalid name / null data / len 0
  // / null out is allowed), kNotFound (no such table), or a HeapTable::Insert
  // error.
  Status Insert(const std::string& table, const std::uint8_t* data,
                std::size_t len, RecordId* out);

  // Reads the record `id` from table `table`. On success `*out_data` points
  // into the pooled page (see the header note) and `*out_len` is its length.
  // Errors: kNotFound (no table, or id not in the table), kInvalidArgument
  // (null out-params / invalid id), or a pool/page error.
  Status Get(const std::string& table, const RecordId& id,
             const std::uint8_t** out_data, std::size_t* out_len);

  // Rewrites record `id` in table `table`. Errors mirror Get().
  Status Update(const std::string& table, const RecordId& id,
                const std::uint8_t* data, std::size_t len);

  // Deletes record `id` from table `table`. Errors mirror Get().
  Status Delete(const std::string& table, const RecordId& id);

  // Scans every live record of table `table`. Returning false from `fn` stops
  // the scan early. Errors: kNotFound (no such table), kInvalidArgument (null
  // callback), or a HeapTable::Scan error.
  Status Scan(const std::string& table, ScanFn fn, void* ctx);

 private:
  QueryExecutor(PageStorage* storage, BufferPool* pool,
                std::unique_ptr<Catalog> catalog)
      : storage_(storage), pool_(pool), catalog_(std::move(catalog)) {}

  // Opens table `name`, translating a missing table into kNotFound.
  Status OpenTable(const std::string& name, std::unique_ptr<HeapTable>* out);

  PageStorage* storage_;
  BufferPool* pool_;
  std::unique_ptr<Catalog> catalog_;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_QUERY_EXECUTOR_H
