// NexusDB — minimal query executor implementation (R1.3).
//
// See include/db/query_executor.h. This file is a thin composition layer over
// Catalog + HeapTable; it re-implements no storage logic.

#include "db/query_executor.h"

namespace nexusdb {
namespace {

inline Status Bad(const char* message) {
  return Status::Error(DbError::kInvalidArgument, message);
}

}  // namespace

Status QueryExecutor::Create(PageStorage* storage, BufferPool* pool,
                             std::unique_ptr<QueryExecutor>* out,
                             Page::PageId* out_catalog_root) {
  if (storage == nullptr || pool == nullptr || out == nullptr) {
    return Bad("QueryExecutor::Create: null storage, pool, or out");
  }
  std::unique_ptr<Catalog> catalog;
  Page::PageId root = Page::kInvalidId;
  Status s = Catalog::Create(storage, pool, &catalog, &root);
  if (!s.ok()) {
    return s;
  }
  if (out_catalog_root != nullptr) {
    *out_catalog_root = root;
  }
  *out = std::unique_ptr<QueryExecutor>(
      new QueryExecutor(storage, pool, std::move(catalog)));
  return Status::Ok();
}

Status QueryExecutor::Open(PageStorage* storage, BufferPool* pool,
                           Page::PageId catalog_root,
                           std::unique_ptr<QueryExecutor>* out) {
  if (storage == nullptr || pool == nullptr || out == nullptr) {
    return Bad("QueryExecutor::Open: null storage, pool, or out");
  }
  std::unique_ptr<Catalog> catalog;
  Status s = Catalog::Open(storage, pool, catalog_root, &catalog);
  if (!s.ok()) {
    return s;
  }
  *out = std::unique_ptr<QueryExecutor>(
      new QueryExecutor(storage, pool, std::move(catalog)));
  return Status::Ok();
}

bool QueryExecutor::TableExists(const std::string& name) const {
  return catalog_->Exists(name);
}

Status QueryExecutor::ListTables(std::vector<std::string>* out) const {
  return catalog_->ListTables(out);
}

Status QueryExecutor::CreateTable(const std::string& name) {
  return catalog_->CreateTable(name, nullptr);
}

Status QueryExecutor::OpenTable(const std::string& name,
                                std::unique_ptr<HeapTable>* out) {
  Page::PageId root = Page::kInvalidId;
  Status s = catalog_->FindTable(name, &root);
  if (!s.ok()) {
    // A syntactically invalid name can never name a table; report "not found"
    // consistently for both so callers see one failure mode.
    return s;
  }
  return catalog_->OpenTable(name, out);
}

Status QueryExecutor::Insert(const std::string& table,
                             const std::uint8_t* data, std::size_t len,
                             RecordId* out) {
  std::unique_ptr<HeapTable> heap;
  Status s = OpenTable(table, &heap);
  if (!s.ok()) {
    return s;
  }
  return heap->Insert(data, len, out);
}

Status QueryExecutor::Get(const std::string& table, const RecordId& id,
                          const std::uint8_t** out_data,
                          std::size_t* out_len) {
  std::unique_ptr<HeapTable> heap;
  Status s = OpenTable(table, &heap);
  if (!s.ok()) {
    return s;
  }
  return heap->Get(id, out_data, out_len);
}

Status QueryExecutor::Update(const std::string& table, const RecordId& id,
                             const std::uint8_t* data, std::size_t len) {
  std::unique_ptr<HeapTable> heap;
  Status s = OpenTable(table, &heap);
  if (!s.ok()) {
    return s;
  }
  return heap->Update(id, data, len);
}

Status QueryExecutor::Delete(const std::string& table, const RecordId& id) {
  std::unique_ptr<HeapTable> heap;
  Status s = OpenTable(table, &heap);
  if (!s.ok()) {
    return s;
  }
  return heap->Delete(id);
}

Status QueryExecutor::Scan(const std::string& table, ScanFn fn, void* ctx) {
  std::unique_ptr<HeapTable> heap;
  Status s = OpenTable(table, &heap);
  if (!s.ok()) {
    return s;
  }
  return heap->Scan(fn, ctx);
}

}  // namespace nexusdb
