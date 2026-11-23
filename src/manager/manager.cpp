// NexusDB — Manager implementation (Digvijay: Manager/Router, R3).
// See include/manager/manager.h for the contract.
#include "manager/manager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "container/cgroup.h"
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
  std::string message = what + ": " + container_strerror(rc);
  if (rc == NEXUS_ERR_PROCESS) {
    const char* detail = cgroup_last_error();
    if (detail != nullptr && detail[0] != '\0')
      message += " (" + std::string(detail) + ")";
  }
  return Err(MapContainerRc(rc), std::move(message));
}

#ifndef _WIN32

// Socket path for a database directory, honoring the engine's relocation
// marker (<dir>/nexusdb.sock.path). The engine records the actual location
// there when the data directory's filesystem cannot host Unix-domain sockets
// (e.g. WSL2 drvfs/9p under /mnt/* rejects bind with EOPNOTSUPP).
std::string ControlSocketPathForDir(const std::string& dir) {
  std::error_code ec;
  const std::filesystem::path marker =
      std::filesystem::path(dir) / "nexusdb.sock.path";
  if (std::filesystem::is_regular_file(marker, ec)) {
    std::ifstream in(marker);
    std::string target;
    if (in && std::getline(in, target)) {
      while (!target.empty() && (target.back() == '\r' || target.back() == '\n' ||
                                 target.back() == ' '))
        target.pop_back();
      if (!target.empty())
        return target;
    }
  }
  return (std::filesystem::path(dir) / "nexusdb.sock").string();
}

bool ProcessAlive(long pid) {
  if (pid <= 0)
    return false;
  if (::kill(static_cast<pid_t>(pid), 0) == 0)
    return true;
  return errno == EPERM;
}

// One connect+send+recv round trip against the engine's control socket, with
// the exact failing syscall and errno in the error message. Callers implement
// their retry policy.
Status EngineExchangeOnce(const std::string& socket_path,
                          const std::string& request, std::string* response) {
  sockaddr_un addr;
  if (socket_path.size() >= sizeof(addr.sun_path))
    return Err(DbError::kInvalidArgument,
               "control socket path is too long: " + socket_path);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    const int e = errno;
    return Err(DbError::kIoError,
               std::string("socket: ") + std::strerror(e) + " (errno=" +
                   std::to_string(e) + ")");
  }
  {
    timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  int send_flags = 0;
#ifdef MSG_NOSIGNAL
  send_flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
  const int no_sigpipe = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                     sizeof(no_sigpipe));
#endif
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int e = errno;
    ::close(fd);
    return Err(DbError::kBadState,
               "connect " + socket_path + ": " + std::strerror(e) +
                   " (errno=" + std::to_string(e) + ")");
  }

  const std::string line = request + "\n";
  std::size_t sent = 0;
  while (sent < line.size()) {
    const ssize_t n =
        ::send(fd, line.data() + sent, line.size() - sent, send_flags);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      const int e = errno;
      ::close(fd);
      return Err(DbError::kIoError,
                 std::string("send: ") + std::strerror(e) + " (errno=" +
                     std::to_string(e) + ")");
    }
    sent += static_cast<std::size_t>(n);
  }

  if (response != nullptr)
    response->clear();
  char buf[1024];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      const int e = errno;
      ::close(fd);
      return Err(DbError::kIoError,
                 std::string("recv: ") + std::strerror(e) + " (errno=" +
                     std::to_string(e) + ")");
    }
    if (n == 0)
      break;
    if (response != nullptr)
      response->append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);

  if (response == nullptr)
    return Status::Ok();
  if (response->empty())
    return Err(DbError::kBadState, "engine closed the connection without a reply");
  return Status::Ok();
}

#endif  // !_WIN32

// Resolves a user/env-provided root to an absolute path so the Manager and
// the db_engine (launched as `--data-dir <root>/<name>`) compute identical
// paths regardless of the working directory of each CLI invocation.
std::string AbsolutePath(const std::string& root) {
  if (root.empty())
    return root;
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(root, ec);
  if (ec || abs.empty())
    return root;
  return abs.lexically_normal().string();
}

std::string ResolveDataRoot() {
  const char* env = std::getenv("NEXUSDB_DATA_ROOT");
  if (env != nullptr && env[0] != '\0')
    return AbsolutePath(env);
  return AbsolutePath("nexusdb_data");
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
  data_root_ = AbsolutePath(data_root_);
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
  cgroup_clear_error();
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
  if (meta.state == NEXUS_RUNNING && !EngineAlive(name)) {
    (void)MarkContainerStopped(name);
    meta.state = NEXUS_STOPPED;
    meta.pid = -1;
  }
  if (meta.state == NEXUS_RUNNING)
    return Err(DbError::kBadState, "container already running: " + name);

  cgroup_clear_error();
  const int rc = container_start(name.c_str());
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "start container " + name);

  const nexus_container_t* c = container_get(name.c_str());
  meta.pid = (c != nullptr) ? c->pid : -1;

#ifndef _WIN32
  // The runtime only guarantees a successful exec, not that the engine
  // finished initializing (a socket failure would otherwise leave a process
  // that looks alive but serves nothing). Confirm the control socket answers
  // a `ping` before reporting RUNNING; a failed initialization reverts the
  // container to STOPPED and propagates the engine's error. Placeholder
  // workloads (no engine) keep the previous lifecycle behavior.
  if (!meta.engine.empty() && meta.pid > 0) {
    bool ready = false;
    std::string detail;
    for (int attempt = 0; attempt < 100; ++attempt) {
      // Re-resolve every attempt: on filesystems that cannot host Unix
      // sockets (WSL2 drvfs/9p under /mnt/*) the engine binds its socket in
      // a runtime directory and writes the real path to
      // <data-dir>/nexusdb.sock.path only after the bind succeeds, so the
      // first probes may still be against the not-yet-relocated default.
      const std::string socket_path = ControlSocketPathForDir(DbDir(name));
      // The engine is a direct child of this process: a non-blocking wait
      // catches a fast exit (e.g. the engine's own startup failures) instead
      // of silently waiting out the whole window.
      int ws = 0;
      const pid_t gone = waitpid((pid_t)meta.pid, &ws, WNOHANG);
      if (gone == (pid_t)meta.pid) {
        detail = WIFEXITED(ws)
                     ? "engine exited with code " + std::to_string(WEXITSTATUS(ws))
                     : "engine terminated by signal " + std::to_string(WTERMSIG(ws));
        break;
      }
      if (gone < 0 && errno == ECHILD) {
        detail = "engine process is gone";
        break;
      }
      std::string pong;
      const Status x = EngineExchangeOnce(socket_path, "ping", &pong);
      if (x.ok()) {
        ready = true;
        break;
      }
      detail = x.message;
      ::usleep(50000);
    }
    if (ready) {
      meta.state = NEXUS_RUNNING;
      return SaveMeta(meta);
    }
    cgroup_clear_error();
    (void)container_stop(name.c_str());
    meta.pid = -1;
    meta.state = NEXUS_STOPPED;
    (void)SaveMeta(meta);
    return Err(DbError::kBadState,
               "start container " + name + ": engine failed to initialize (" +
                   detail + "); the container was stopped");
  }
#endif

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

  cgroup_clear_error();
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

  cgroup_clear_error();
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
  cgroup_clear_error();
  const int rc = container_status(name.c_str(), buf, sizeof(buf));
  if (rc != NEXUS_OK)
    return FromContainerRc(rc, "status container " + name);
  if (out != nullptr)
    *out = buf;
  return Status::Ok();
}

Status Manager::ListContainers(std::vector<ContainerInfo>* out) const {
  if (out == nullptr)
    return Err(DbError::kInvalidArgument, "null output");
  out->clear();
  std::error_code ec;
  const std::filesystem::path dir(MetaDir());
  if (!std::filesystem::is_directory(dir, ec))
    return Status::Ok();
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec)
      break;
    std::error_code file_ec;
    if (!entry.is_regular_file(file_ec) ||
        entry.path().extension() != ".meta")
      continue;
    const std::string name = entry.path().stem().string();
    ContainerMeta meta;
    if (!LoadMeta(name, &meta))
      continue;
    ContainerInfo info;
    info.name = meta.name;
    info.id = meta.id;
    info.state = meta.state;
    info.pid = meta.pid;
    info.memory_bytes = meta.memory;
    info.cpu_percent = meta.cpu;
    info.engine = meta.engine;
    out->push_back(std::move(info));
  }
  std::sort(out->begin(), out->end(),
            [](const ContainerInfo& a, const ContainerInfo& b) {
              return a.name < b.name;
            });
  return Status::Ok();
}

bool Manager::ContainerIsRunning(const std::string& name) const {
  ContainerMeta meta;
  return LoadMeta(name, &meta) && meta.state == NEXUS_RUNNING;
}

bool Manager::EngineAlive(const std::string& name) const {
#ifndef _WIN32
  if (!IsValidName(name))
    return false;
  ContainerMeta meta;
  if (LoadMeta(name, &meta) && meta.state == NEXUS_RUNNING &&
      ProcessAlive(meta.pid))
    return true;
  const std::string socket_path = ControlSocketPathForDir(DbDir(name));
  if (socket_path.size() >= sizeof(sockaddr_un::sun_path))
    return false;
  std::string pong;
  return EngineExchangeOnce(socket_path, "ping", &pong).ok() &&
         pong.find("OK") != std::string::npos;
#else
  (void)name;
  return false;
#endif
}

Status Manager::MarkContainerStopped(const std::string& name) const {
  ContainerMeta meta;
  if (!LoadMeta(name, &meta))
    return Err(DbError::kNotFound, "container not found: " + name);
  meta.pid = -1;
  meta.state = NEXUS_STOPPED;
  return SaveMeta(meta);
}

Status Manager::EngineRequest(const std::string& name,
                              const std::string& request,
                              std::string* response) const {
  if (!IsValidName(name))
    return Err(DbError::kInvalidArgument, "invalid container name: " + name);
#ifndef _WIN32
  const std::string socket_path = ControlSocketPathForDir(DbDir(name));
  if (socket_path.size() >= sizeof(sockaddr_un::sun_path))
    return Err(DbError::kInvalidArgument,
               "control socket path is too long: " + socket_path);

  ContainerMeta meta;
  const bool tracked = LoadMeta(name, &meta);
  std::string last;
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (tracked && meta.state == NEXUS_RUNNING && !EngineAlive(name)) {
      last = "engine process is gone";
      break;
    }
    const Status s = EngineExchangeOnce(socket_path, request, response);
    if (s.ok())
      return s;
    last = s.message;
    ::usleep(50000);
  }
  return Err(
      DbError::kBadState,
      "engine control socket unavailable for " + name + " at " + socket_path +
          " (" + last + "; is the container actually running?)");
#else
  (void)request;
  (void)response;
  return Err(DbError::kBadState,
             "the engine control socket is not available on this platform");
#endif
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
  if (EngineAlive(name))
    return Err(DbError::kBadState,
               "engine is running for " + name +
                   "; stop it before direct database access");

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
  if (out == nullptr)
    return Err(DbError::kInvalidArgument, "scan output pointer is null");
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
