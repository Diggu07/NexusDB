// NexusDB — fixed-size database Page (Digvijay: Database Engine, Phase 1).
//
// A Page is one in-memory database page: an id, a kPageSize byte buffer,
// and a dirty flag. It is a deliberately low-level storage abstraction.
//
// A Page knows NOTHING about: SQL, tables, indexes, transactions, WAL,
// the Buffer Pool, LRU, the Query Processor, or the Manager. Dirty-state
// lifecycle (who sets/clears it, when it is written back) belongs to the
// future Buffer Pool; PageStorage never touches the flag.

#ifndef NEXUSDB_DB_PAGE_H
#define NEXUSDB_DB_PAGE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/config.h"

namespace nexusdb {

class Page {
 public:
  using PageId = std::uint32_t;

  // Sentinel for "no page". Never a valid on-disk page id: valid ids are
  // dense from 0, so this value is unreachable via AllocatePage.
  static constexpr PageId kInvalidId = 0xFFFFFFFFu;

  // Default: invalid id, zeroed data, clean.
  Page();

  // Page with the given id, zeroed data, clean.
  explicit Page(PageId id);

  PageId id() const { return id_; }
  bool IsValid() const { return id_ != kInvalidId; }

  static constexpr std::size_t size() { return kPageSize; }

  std::uint8_t* data() { return data_.data(); }
  const std::uint8_t* data() const { return data_.data(); }

  bool IsDirty() const { return dirty_; }
  void MarkDirty() { dirty_ = true; }
  void ClearDirty() { dirty_ = false; }

  // Reinitialize as page `id` with zeroed data and a clean flag.
  void Reset(PageId id);

 private:
  PageId id_;
  bool dirty_;
  std::array<std::uint8_t, kPageSize> data_;
};

}  // namespace nexusdb

#endif  // NEXUSDB_DB_PAGE_H
