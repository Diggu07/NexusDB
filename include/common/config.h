// NexusDB — shared engine configuration (Digvijay: Database Engine).
//
// Only values needed by the current implementation phase live here.
// New tunables are added alongside the phase that introduces them.

#ifndef NEXUSDB_COMMON_CONFIG_H
#define NEXUSDB_COMMON_CONFIG_H

#include <cstdint>

namespace nexusdb {

// Fixed database page size: 4096 bytes (4 KiB).
//
// One OS memory page on all supported targets (x86-64 Linux/WSL2), so a
// database page maps 1:1 onto a memory page. Every page in the database
// file occupies exactly kPageSize bytes; the byte offset of page N is
// N * kPageSize. This single constant sizes Page::data_, the on-disk
// layout in PageStorage, and (later) Buffer Pool frames.
constexpr std::uint32_t kPageSize = 4096;

}  // namespace nexusdb

#endif  // NEXUSDB_COMMON_CONFIG_H
