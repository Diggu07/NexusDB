// NexusDB — Manager implementation (Digvijay: Manager/Router, R3).
// See include/manager/manager.h for the contract.
#include "manager/manager.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#include "container/container.h"
#include "db/catalog.h"
#include "db/query_executor.h"

namespace nexusdb {
namespace {

constexpr std::size_t kDefaultPoolFrames = 64;

Status Err(DbError code, std::string message) {
  return Status::Error(code, std::move(message));
}

DbError MapContainerRc(int rc) {
  switch (rc) {
    case NEXUS_OK:
      return DbError::kOk;
    case NEXUS_ERR_INVALID:
      return DbError::kInvalidArgument;
    case NEXUS_ERR_NOT_FOUND:
      return DbError::kNotFound;
    case NEXUS_ERR_NOMEM:
      return DbError::kOutOfMemory;
    case NEXUS_ERR_PROCESS:
      return DbError::kIoError;
    case NEXUS_ERR_EXISTS:
    case NEXUS_ERR_STATE:
    case NEXUS_ERR_UNSUPPORTED:
    default:
      return DbError::kBadState;
  }
}

Status FromContainerRc(int rc, const std::string& what) {
  if (rc == NEXUS_OK)
    return Status::Ok();
  return Err(MapContainerRc(rc),
             what + ": " + container_strerror(rc));
}

std::string ResolveDataRoot() {
  const char* env = std::getenv("NEXUSDB_DATA_ROOT");
  if (env != nullptr && env[0] != '\0')
    return std::string(env);
  return std::string("nexusdb_data");
}

// --- scan collection -------------------------------------------------------

struct ScanSink {
  std::vector<ScanRecord>* out = nullptr;
};

bool CollectScan(const QueryExecutor::RecordId& id, const std::uint8_t* data,
                 std::size_t len, void* ctx) {
  ScanSink* sink = static_cast<ScanSink*>(ctx);
  ScanRecord rec;
  rec.page = id.page;
  rec.slot = id.slot;
  if (len > 0)
    rec.data.assign(data, data + len);
  sink->out->push_back(std::move(rec));
  return true;
}

}  // namespace

// Persisted container metadata. Written as `key=value` lines; the on-disk record
// (not the process-local registry) is the source of truth across CLI invocations.
struct Manager::ContainerMeta {
  std::string name;
  std::string id;
  unsigned long memory = 0;
  unsigned int cpu = 0;
  std::string engine;
  std::string cgroup;
  long pid = -1;
  int state = NEXUS_CREATED;
};

Manager::Manager() : data_root_(ResolveDataRoot()) {}

Manager::Manager(std::string data_root) : data_root_(std::move(data_root)) {
  if (data_root_.empty())
    data_root_ = ResolveDataRoot();
}

bool Manager::IsValidName(const std::string& name) {
  if (name.empty() || name.size() >= NEXUS_NAME_MAX)
    return false;
  for (char ch : name) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (!std::isalnum(u) && ch != '_' && ch != '-')
      return false;
  }
  return true;
}

std::string Manager::DbDir(const std::string& name) const {
  return (std::filesystem::path(data_root_) / name).string();
}

std::string Manager::MetaDir() const {
  return (std::filesystem::path(data_root_) / ".nexus" / "containers").string();
}

std::string Manager::MetaPath(const std::string& name) const {
  return (std::filesystem::path(MetaDir()) / (name + ".meta")).string();
}

Status Manager::Init() {
  std::error_code ec;
  std::filesystem::create_directories(data_root_, ec);
  std::filesystem::create_directories(MetaDir(), ec);
  if (!std::filesystem::is_directory(MetaDir(), ec))
    return Err(DbError::kIoError,
               "cannot create metadata directory: " + MetaDir());
  return Status::Ok();
}

// --- metadata I/O ----------------------------------------------------------

bool Manager::LoadMeta(const std::string& name, ContainerMeta* out) const {
  std::ifstream in(MetaPath(name));
  if (!in)
    return false;
  ContainerMeta meta;
  meta.name = name;
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    const std::string val = line.substr(eq + 1);
    if (key == "name")
      meta.name = val;
    else if (key == "id")
      meta.id = val;
    else if (key == "memory")
      meta.memory = std::strtoul(val.c_str(), nullptr, 10);
    else if (key == "cpu")
      meta.cpu = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
    else if (key == "engine")
      meta.engine = val;
    else if (key == "cgroup")
      meta.cgroup = val;
    else if (key == "pid")
      meta.pid = std::strtol(val.c_str(), nullptr, 10);
    else if (key == "state")
      meta.state = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
  }
  *out = std::move(meta);
  return true;
}

Status Manager::SaveMeta(const ContainerMeta& meta) const {
  std::error_code ec;
  std::filesystem::create_directories(MetaDir(), ec);
  std::ofstream out(MetaPath(meta.name), std::ios::trunc);
  if (!out)
    return Err(DbError::kIoError, "cannot write metadata for " + meta.name);
  out << "name=" << meta.name << "\n"
      << "id=" << meta.id << "\n"
      << "memory=" << meta.memory << "\n"
      << "cpu=" << meta.cpu << "\n"
      << "engine=" << meta.engine << "\n"
      << "cgroup=" << meta.cgroup << "\n"
      << "pid=" << meta.pid << "\n"
      << "state=" << meta.state << "\n";
  if (!out)
    return Err(DbError::kIoError, "failed writing metadata for " + meta.name);
  return Status::Ok();
}

Status Manager::RemoveMeta(const std::string& name) const {
  std::error_code ec;
  std::filesystem::remove(MetaPath(name), ec);
  // A missing file means the container was never registered; not an error.
  return Status::Ok();
}

Status Manager::Adopt(const std::string& name, ContainerMeta* out) const {
  ContainerMeta meta;
  if (!LoadMeta(name, &meta))
    return Err(DbError::kNotFound, "no such container: " + name);
  const int rc = container_adopt(
      meta.name.c_str(), meta.id.c_str(), meta.memory, meta.cpu,
      meta.engine.empty() ? "" : meta.engine.c_str(),
      meta.cgroup.empty() ? "" : meta.cgroup.c_str(), meta.pid, meta.state);
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "adopt container " + name);
  if (out != nullptr)
    *out = std::move(meta);
  return Status::Ok();
}

// --- container lifecycle ---------------------------------------------------

Status Manager::ContainerCreate(const std::string& name,
                                unsigned long memory_bytes,
                                unsigned int cpu_percent,
                                const std::string& engine) {
  if (!IsValidName(name))
    return Err(DbError::kInvalidArgument, "invalid container name: " + name);
  ContainerMeta existing;
  if (LoadMeta(name, &existing))
    return Err(DbError::kBadState, "container already exists: " + name);

  Status s = Init();
  if (!s.ok())
    return s;

  const char* engine_arg = engine.empty() ? nullptr : engine.c_str();
  const int rc = container_create_ex(name.c_str(), memory_bytes, cpu_percent,
                                     engine_arg, nullptr);
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "create container " + name);

  ContainerMeta meta;
  meta.name = name;
  meta.memory = memory_bytes;
  meta.cpu = cpu_percent;
  meta.engine = engine;
  meta.pid = -1;
  meta.state = NEXUS_CREATED;
  const nexus_container_t* c = container_get(name.c_str());
  if (c != nullptr) {
    meta.id = c->id;
    meta.cgroup = c->cgroup_path;
  }
  return SaveMeta(meta);
}

Status Manager::ContainerStart(const std::string& name) {
  if (!IsValidName(name))
    return Err(DbError::kInvalidArgument, "invalid container name: " + name);
  ContainerMeta meta;
  Status s = Adopt(name, &meta);
  if (!s.ok())
    return s;
  if (meta.state == NEXUS_RUNNING)
    return Err(DbError::kBadState, "container already running: " + name);

  const int rc = container_start(name.c_str());
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "start container " + name);

  const nexus_container_t* c = container_get(name.c_str());
  meta.pid = (c != nullptr) ? c->pid : -1;
  meta.state = NEXUS_RUNNING;
  return SaveMeta(meta);
}

Status Manager::ContainerStop(const std::string& name) {
  if (!IsValidName(name))
    return Err(DbError::kInvalidArgument, "invalid container name: " + name);
  ContainerMeta meta;
  Status s = Adopt(name, &meta);
  if (!s.ok())
    return s;

  const int rc = container_stop(name.c_str());
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "stop container " + name);

  meta.pid = -1;
  meta.state = NEXUS_STOPPED;
  return SaveMeta(meta);
}

Status Manager::ContainerDelete(const std::string& name) {
  if (!IsValidName(name))
    return Err(DbError::kInvalidArgument, "invalid container name: " + name);
  ContainerMeta meta;
  Status s = Adopt(name, &meta);
  if (!s.ok())
    return s;

  const int rc = container_delete(name.c_str());
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "delete container " + name);
  return RemoveMeta(name);
}

Status Manager::ContainerStatus(const std::string& name, std::string* out) {
  if (!IsValidName(name))
    return Err(DbError::kInvalidArgument, "invalid container name: " + name);
  ContainerMeta meta;
  Status s = Adopt(name, &meta);
  if (!s.ok())
    return s;

  char buf[512];
  const int rc = container_status(name.c_str(), buf, sizeof(buf));
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "status container " + name);
  if (out != nullptr)
    *out = buf;
  return Status::Ok();
}

// --- database operations ---------------------------------------------------

Status Manager::OpenDb(const std::string& name,
                       std::unique_ptr<Database>* out) const {
  if (!IsValidName(name))
    return Err(DbError::kInvalidArgument, "invalid database name: " + name);

  ContainerMeta meta;
  if (LoadMeta(name, &meta) && meta.state == NEXUS_RUNNING)
    return Err(DbError::kBadState,
               "container " + name +
                   " is running; stop it before direct database access "
                   "(no client protocol exists)");

  const std::filesystem::path dir(DbDir(name));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec))
    return Err(DbError::kIoError, "cannot create database directory: " + dir.string());

  const std::string db_path = (dir / "nexusdb.db").string();
  const std::string wal_path = (dir / "nexusdb.wal").string();
  return Database::Open(db_path, wal_path, kDefaultPoolFrames, out);
}

Status Manager::DbInit(const std::string& name) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  return db->Close();
}

Status Manager::TableCreate(const std::string& name, const std::string& table) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  s = db->executor().CreateTable(table);
  const Status close = db->Close();
  return s.ok() ? close : s;
}

Status Manager::TableList(const std::string& name,
                          std::vector<std::string>* out) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  s = db->executor().ListTables(out);
  const Status close = db->Close();
  return s.ok() ? close : s;
}

Status Manager::Insert(const std::string& name, const std::string& table,
                       const std::uint8_t* data, std::size_t len,
                       Page::PageId* page, std::uint16_t* slot) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  QueryExecutor::RecordId id;
  s = db->executor().Insert(table, data, len, &id);
  const Status close = db->Close();
  if (!s.ok())
    return s;
  if (!close.ok())
    return close;
  if (page != nullptr)
    *page = id.page;
  if (slot != nullptr)
    *slot = id.slot;
  return Status::Ok();
}

Status Manager::Get(const std::string& name, const std::string& table,
                    Page::PageId page, std::uint16_t slot,
                    std::vector<std::uint8_t>* out) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  QueryExecutor::RecordId id;
  id.page = page;
  id.slot = slot;
  const std::uint8_t* data = nullptr;
  std::size_t len = 0;
  s = db->executor().Get(table, id, &data, &len);
  if (s.ok() && out != nullptr) {
    if (len > 0)
      out->assign(data, data + len);
    else
      out->clear();
  }
  const Status close = db->Close();
  return s.ok() ? close : s;
}

Status Manager::Update(const std::string& name, const std::string& table,
                       Page::PageId page, std::uint16_t slot,
                       const std::uint8_t* data, std::size_t len) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  QueryExecutor::RecordId id;
  id.page = page;
  id.slot = slot;
  s = db->executor().Update(table, id, data, len);
  const Status close = db->Close();
  return s.ok() ? close : s;
}

Status Manager::Delete(const std::string& name, const std::string& table,
                       Page::PageId page, std::uint16_t slot) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  QueryExecutor::RecordId id;
  id.page = page;
  id.slot = slot;
  s = db->executor().Delete(table, id);
  const Status close = db->Close();
  return s.ok() ? close : s;
}

Status Manager::Scan(const std::string& name, const std::string& table,
                     std::vector<ScanRecord>* out) {
  std::unique_ptr<Database> db;
  Status s = OpenDb(name, &db);
  if (!s.ok())
    return s;
  if (out != nullptr)
    out->clear();
  ScanSink sink;
  sink.out = out;
  s = db->executor().Scan(table, CollectScan, &sink);
  const Status close = db->Close();
  return s.ok() ? close : s;
}

}  // namespace nexusdb
