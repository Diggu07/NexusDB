// NexusDB — Manager / Router coordination layer (Digvijay: Manager/Router, R3).
//
// The Manager is the single place that knows how a *named database container*
// maps onto (a) the Linux container runtime and (b) the on-disk database. It
// keeps the CLI and the Router free of lifecycle and storage logic.
//
// Two, clearly separated responsibilities
// ---------------------------------------
//   1. Container lifecycle. Routed THROUGH the existing container runtime
//      (include/container/container.h): create/start/stop/delete/status. Because
//      that registry is process-local and the CLI runs as a fresh process per
//      command, the Manager also PERSISTS each container's metadata to
//      <data_root>/.nexus/containers/<name>.meta and re-adopts it into the
//      runtime (container_adopt) before operating. Persistent metadata is the
//      source of truth, never the process-local registry alone.
//
//   2. Database operations. Routed through Database -> Catalog/QueryExecutor,
//      so every open runs WAL recovery first and every mutation is made durable
//      by a WAL-ordered Close(). The database for name N lives in
//      <data_root>/N/nexusdb.db (exactly the directory the runtime passes to
//      the configured db_engine as `--data-dir <root>/N`).
//
// No client/server protocol exists. A running container's db_engine holds the
// database open, so database commands REFUSE to run while the container is
// RUNNING (they would race the engine's page writes). Stop the container first.
//
// Error convention: every fallible call returns Status (common/error.h); no
// exception escapes. kInvalidArgument/kNotFound carry user-facing meaning; the
// rest are operational failures.

#ifndef NEXUSDB_MANAGER_MANAGER_H
#define NEXUSDB_MANAGER_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/error.h"
#include "db/database.h"
#include "db/page.h"

namespace nexusdb {

// One scanned record, copied out of the pooled page so it stays valid after the
// database is closed.
struct ScanRecord {
  Page::PageId page = Page::kInvalidId;
  std::uint16_t slot = 0;
  std::vector<std::uint8_t> data;
};

class Manager {
 public:
  // Uses NEXUSDB_DATA_ROOT when set and non-empty, else "./nexusdb_data" --
  // the same resolution the container runtime uses for its workloads.
  Manager();
  explicit Manager(std::string data_root);

  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

  // Creates the metadata directory. Idempotent. Call before other operations.
  Status Init();

  const std::string& data_root() const { return data_root_; }

  // Absolute/relative directory holding database `name`'s files.
  std::string DbDir(const std::string& name) const;

  // ---- Container lifecycle (routed to the container runtime) ----

  // Registers and provisions a container. `engine` is the executable the
  // runtime launches on start ("" for the built-in placeholder). Fails if a
  // container named `name` is already registered.
  Status ContainerCreate(const std::string& name, unsigned long memory_bytes,
                         unsigned int cpu_percent, const std::string& engine);

  Status ContainerStart(const std::string& name);
  Status ContainerStop(const std::string& name);
  Status ContainerDelete(const std::string& name);
  Status ContainerStatus(const std::string& name, std::string* out);

  // ---- Database operations (routed through Database/QueryExecutor) ----

  // Creates the database files and an empty catalog for `name` (idempotent: an
  // existing database is opened with recovery and left intact).
  Status DbInit(const std::string& name);

  Status TableCreate(const std::string& name, const std::string& table);
  Status TableList(const std::string& name, std::vector<std::string>* out);

  Status Insert(const std::string& name, const std::string& table,
                const std::uint8_t* data, std::size_t len, Page::PageId* page,
                std::uint16_t* slot);
  Status Get(const std::string& name, const std::string& table,
             Page::PageId page, std::uint16_t slot,
             std::vector<std::uint8_t>* out);
  Status Update(const std::string& name, const std::string& table,
                Page::PageId page, std::uint16_t slot,
                const std::uint8_t* data, std::size_t len);
  Status Delete(const std::string& name, const std::string& table,
                Page::PageId page, std::uint16_t slot);
  Status Scan(const std::string& name, const std::string& table,
              std::vector<ScanRecord>* out);

  // True when `name` is a valid database/container identifier (path-safe).
  static bool IsValidName(const std::string& name);

 private:
  struct ContainerMeta;

  std::string MetaDir() const;
  std::string MetaPath(const std::string& name) const;

  bool LoadMeta(const std::string& name, ContainerMeta* out) const;
  Status SaveMeta(const ContainerMeta& meta) const;
  Status RemoveMeta(const std::string& name) const;

  // Loads persisted metadata and adopts it into the runtime registry so the
  // normal lifecycle functions can be used in this process.
  Status Adopt(const std::string& name, ContainerMeta* out) const;

  // Opens the database for `name` (running recovery). Refuses while a RUNNING
  // container holds it. `out` receives the open database.
  Status OpenDb(const std::string& name, std::unique_ptr<Database>* out) const;

  std::string data_root_;
};

}  // namespace nexusdb

#endif  // NEXUSDB_MANAGER_MANAGER_H
