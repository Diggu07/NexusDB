// NexusDB — minimal long-running database-engine executable (P3).
//
// This is the executable the Container Engine launches as a container's
// workload. It is deliberately minimal and honest: there is NO SQL parser and
// NO network listener. It opens the full Database (PageStorage + WAL +
// BufferPool + Catalog/QueryExecutor), running WAL recovery on startup, keeps
// the database open, and continuously adapts the pool to the container's real
// cgroup memory pressure. It stays alive until the container asks it to stop,
// at which point it flushes (WAL-ordered) and closes cleanly. An abrupt
// termination leaves the WAL for the next Open.
//
// Contract
// --------
//   Usage:
//     db_engine [--data-dir <dir>] [--buffer-pool <frames>]
//
//     --data-dir <dir>     directory holding the database file `nexusdb.db`
//                          (created if missing). Default: $NEXUSDB_DATA_DIR if
//                          set, else ./nexusdb_data. Resolved to an absolute
//                          path at startup (see socket relocation below).
//     --buffer-pool <n>    buffer-pool capacity in pages (default 64, min 1).
//     --socket <path>      control socket path (default <data-dir>/nexusdb.sock,
//                          or "none" to disable). POSIX only. An explicit path
//                          that cannot be bound is a hard error; for the
//                          default path the engine relocates the socket (see
//                          the control-socket paragraph below).
//
//   Control socket (POSIX):
//     The engine is not a network server, but while it holds the database open
//     it serves a small newline-delimited Unix-domain-socket protocol so the
//     `nexusdb` CLI can issue database commands to the RUNNING engine instead of
//     opening the same file a second time (which would race the engine). This is
//     the smallest sound IPC for the existing file-based architecture.
//
//       ping                         -> "OK pong"
//       stats                        -> "OK" + pages/pool/cache/latency/adaptation lines
//       init                         -> "OK initialized" (already open)
//       table create <name>          -> "OK created table <name>"
//       table list                   -> "OK" + one table name per line
//       insert <table> <hex>         -> "OK page=<p> slot=<s>"
//       get <table> <page> <slot>    -> "OK page=<p> slot=<s> len=<n> hex=<hex>"
//       update <table> <page> <slot> <hex> -> "OK updated page=<p> slot=<s>"
//       delete <table> <page> <slot> -> "OK deleted page=<p> slot=<s>"
//       scan <table>                 -> "OK" + "page=.. slot=.. len=.. hex=.." lines
//       quit                         -> "OK terminating" then clean shutdown
//
//     Errors are "ERR <code> <message>" with code 1 (operational), 3 (invalid
//     argument) or 4 (not found), matching the CLI exit codes.
//
//   Startup:
//     1. parse arguments (unknown/missing-value -> usage error);
//     2. create the data directory (recursively);
//     3. Database::Open(<dir>/nexusdb.db, <dir>/nexusdb.wal, pool) -- this
//        opens the log, replays it into storage BEFORE the pool exists, then
//        checkpoints and opens the Catalog/QueryExecutor; on a fresh database
//        it reserves header page 0 and creates a catalog at page 1;
//     4. bind the control socket, print a ready line, and serve requests until
//        a shutdown request.
//
//   Shutdown (SIGTERM / SIGINT): Database::Close() flushAlls the pool in
//   WAL order, resets/closes the log, and closes the storage, then exits 0. The
//   process does NOT busy-loop: the control loop blocks in poll(), and without a
//   socket it waits in a timed poll() (POSIX) / Sleep(1000) (Windows) between
//   checks of the stop flag.
//
//   Exit codes:
//     0  clean shutdown
//     2  usage / argument error
//     3  storage open / initialization failure
//     4  buffer-pool creation failure (or a pool operation failed at startup)
//     5  flush / close failure during shutdown
//     6  control socket required but could not be created (all candidate paths
//        failed; the database is closed cleanly and no ready line is printed)
//
// Platform: builds on Linux/WSL2 (the real target) and MinGW/Windows. A
// windowless Windows child cannot receive SIGTERM/SIGINT, so the container
// terminates it (TerminateProcess); the graceful flush path is the POSIX one.
// The Container Engine only launches a configured db_engine on POSIX (Windows
// returns NEXUS_ERR_UNSUPPORTED); standalone, the binary still runs on Windows.
//
// Control-socket relocation (POSIX)
// ---------------------------------
//   The default socket lives beside the database, but not every filesystem can
//   host an AF_UNIX socket: WSL2's drvfs/9p mounts (/mnt/<drive>) reject bind()
//   with EOPNOTSUPP, as do several network filesystems. When the default bind
//   fails with such an errno the engine relocates the socket into a directory
//   that supports it -- $NEXUSDB_SOCKET_DIR if set, else $XDG_RUNTIME_DIR/nexusdb,
//   else /tmp/nexusdb-<uid> -- under a name derived from the absolute data-dir
//   path, and records the real location in <data-dir>/nexusdb.sock.path so the
//   CLI/Manager can find it. The marker and socket are removed on clean
//   shutdown.

#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "bridge/adaptation_policy.h"
#include "bridge/buffer_pool_adapter.h"
#include "bridge/resource_monitor.h"
#include "common/config.h"
#include "common/error.h"
#include "container/cgroup.h"
#include "db/database.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/query_executor.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitStorage = 3;
constexpr int kExitPool = 4;
constexpr int kExitShutdown = 5;
// Control socket was required (--socket not "none") but no candidate path
// could be bound. The cause (strerror + errno) is on stderr.
constexpr int kExitSocket = 6;

// Largest accepted --buffer-pool value, so a typo cannot request an absurd
// allocation. Well above any realistic small database.
constexpr std::size_t kMaxPoolFrames = 1u << 20;
constexpr int kAdaptIntervalMs = 500;

struct AdaptationState {
  nexus_policy_t policy{};
  nexus_policy_config_t config{};
  std::string container;
  std::string cgroup_dir;
  bool enabled = false;
};

struct AdaptationStatus {
  bool enabled = false;
  bool available = false;
  nexus_adapt_action_t action = NEXUS_ADAPT_HOLD;
  nexus_adapt_reason_t reason = NEXUS_REASON_NO_METRICS;
  unsigned long target_frames = 0;
  double pressure = 0.0;
  unsigned long applications = 0;
  int last_error = NEXUS_OK;
};

AdaptationState g_adaptation;
AdaptationStatus g_adapt_status;
std::mutex g_db_mutex;
volatile std::sig_atomic_t g_stop = 0;

std::string ContainerNameFromDataDir(const std::string& data_dir) {
  std::string name = std::filesystem::path(data_dir).filename().string();
  if (name.empty() || name == "." || name == "..")
    return std::string();
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!std::isalnum(u) && c != '-' && c != '_')
      return std::string();
  }
  return name;
}

std::string CgroupDirFor(const std::string& name) {
  const char* root = cgroup_root();
  if (root == nullptr || root[0] == '\0')
    return std::string();
  return (std::filesystem::path(root) / name).lexically_normal().string();
}

void RunAdaptation(nexusdb::Database* db) {
#ifndef _WIN32
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGTERM);
  (void)pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
  nexus_resource_snapshot_t snapshot;
  while (!g_stop) {
    for (int tick = 0; tick < kAdaptIntervalMs / 20 && !g_stop; ++tick)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (g_stop)
      break;

    const int read_rc =
        nexus_bridge_read_cgroup(g_adaptation.cgroup_dir.c_str(), &snapshot);
    {
      std::lock_guard<std::mutex> lock(g_db_mutex);
      g_adapt_status.available = (read_rc == NEXUS_OK && snapshot.valid != 0);
      g_adapt_status.last_error = read_rc;
      if (!g_adapt_status.available) {
        g_adapt_status.action = NEXUS_ADAPT_HOLD;
        g_adapt_status.reason = NEXUS_REASON_NO_METRICS;
        continue;
      }

      nexus_adapt_decision_t decision;
      const int rc = nexus_policy_apply(
          &g_adaptation.policy, &g_adaptation.config, &snapshot,
          static_cast<unsigned long>(db->pool()->capacity()),
          nexus_bridge_buffer_pool_request, db->pool(), &decision);
      g_adapt_status.action = decision.action;
      g_adapt_status.reason = decision.reason;
      g_adapt_status.target_frames = decision.target_frames;
      g_adapt_status.pressure = decision.pressure;
      g_adapt_status.last_error = rc;
      if (rc == NEXUS_OK &&
          (decision.action == NEXUS_ADAPT_GROW ||
           decision.action == NEXUS_ADAPT_SHRINK)) {
        ++g_adapt_status.applications;
      }
    }
  }
}

void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [--data-dir <dir>] [--buffer-pool <frames>] "
               "[--socket <path>]\n"
               "  --data-dir <dir>    directory holding nexusdb.db "
               "(default: $NEXUSDB_DATA_DIR or ./nexusdb_data)\n"
               "  --buffer-pool <n>   buffer-pool capacity in pages "
               "(default: 64)\n"
               "  --socket <path>     control socket for the CLI "
               "(default: <data-dir>/nexusdb.sock, \"none\" disables)\n",
               prog);
}

bool ParseCount(const char* text, std::size_t* out) {
  if (text == nullptr || *text == '\0')
    return false;
  std::size_t value = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9')
      return false;
    value = value * 10 + static_cast<std::size_t>(*p - '0');
    if (value > kMaxPoolFrames)
      return false;
  }
  *out = value;
  return true;
}

long CurrentPid() {
#ifdef _WIN32
  return static_cast<long>(GetCurrentProcessId());
#else
  return static_cast<long>(getpid());
#endif
}

// Waits briefly without spinning so the caller can re-check the stop flag even
// when a signal was delivered just before the wait began.
void IdleOnce() {
#ifdef _WIN32
  Sleep(1000);
#else
  pollfd p;
  p.fd = -1;
  p.events = 0;
  p.revents = 0;
  (void)::poll(&p, 0, 200);
#endif
}

}  // namespace

// Stop flag + handler. The handler must be async-signal-safe: it only sets a
// sig_atomic_t; all flushing happens after the idle loop returns.
extern "C" void NexusEngineOnSignal(int sig) {
  (void)sig;
  g_stop = 1;
}

// ---------------------------------------------------------------------------
// Control-socket protocol (POSIX only). See the contract at the top of this
// file. Everything here runs on the engine's single thread, so it can use the
// already-open Database directly without extra locking.
// ---------------------------------------------------------------------------
#ifndef _WIN32
namespace {

constexpr std::size_t kMaxRequest = 4096;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

using Clock = std::chrono::steady_clock;
double g_last_query_latency_ms = -1.0;

std::string HexEncode(const std::uint8_t* data, std::size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0xF]);
    out.push_back(kHex[data[i] & 0xF]);
  }
  return out;
}

int HexVal(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool HexDecode(const std::string& text, std::vector<std::uint8_t>* out) {
  if (text.empty() || text.size() % 2 != 0)
    return false;
  out->clear();
  out->reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = HexVal(text[i]);
    const int lo = HexVal(text[i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out->push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

bool ParseU64(const std::string& text, std::uint64_t* out) {
  if (text.empty())
    return false;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

std::vector<std::string> Tokenize(const std::string& line) {
  std::istringstream in(line);
  std::vector<std::string> out;
  std::string token;
  while (in >> token)
    out.push_back(token);
  return out;
}

std::string Ok(const std::string& text = std::string()) {
  return text.empty() ? std::string("OK\n") : "OK " + text + "\n";
}

void RecordQueryLatency(Clock::time_point start) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      Clock::now() - start)
                      .count();
  g_last_query_latency_ms = static_cast<double>(us) / 1000.0;
}

std::string QueryOk(const std::string& text, Clock::time_point start) {
  RecordQueryLatency(start);
  return Ok(text);
}

std::string QueryLines(const std::string& body, Clock::time_point start) {
  RecordQueryLatency(start);
  return "OK\n" + body;
}

int ExitCode(const nexusdb::Status& s) {
  using nexusdb::DbError;
  switch (s.code) {
    case DbError::kInvalidArgument:
      return 3;
    case DbError::kNotFound:
      return 4;
    case DbError::kOk:
      return 0;
    default:
      return 1;
  }
}

std::string Err(const nexusdb::Status& s) {
  const std::string message =
      s.message.empty() ? std::string(nexusdb::DbErrorString(s.code))
                        : s.message;
  return "ERR " + std::to_string(ExitCode(s)) + " " + message + "\n";
}

bool AppendScan(const nexusdb::QueryExecutor::RecordId& id,
                const std::uint8_t* data, std::size_t len, void* ctx) {
  auto* out = static_cast<std::string*>(ctx);
  *out += "page=" + std::to_string(id.page);
  *out += " slot=" + std::to_string(id.slot);
  *out += " len=" + std::to_string(len);
  *out += " hex=" + HexEncode(data, len);
  *out += '\n';
  return true;
}

// Executes one request line against the open database. Returns the complete
// response. Sets *quit when the engine should shut down after replying.
std::string DispatchCommand(nexusdb::Database* db, const std::string& line,
                            bool* quit) {
  using nexusdb::Page;
  using nexusdb::QueryExecutor;
  using nexusdb::Status;

  const std::vector<std::string> a = Tokenize(line);
  if (a.empty())
    return "ERR 3 empty request\n";
  const auto started = Clock::now();

  if (a[0] == "ping")
    return Ok("pong");

  if (a[0] == "quit") {
    *quit = true;
    return Ok("terminating");
  }

  if (a[0] == "init")
    return Ok("initialized");

  if (a[0] == "stats") {
    std::uint32_t pages = 0;
    const Status s = db->storage()->NumPages(&pages);
    if (!s.ok())
      return Err(s);
    std::string out = "OK\n";
    out += "pages " + std::to_string(pages) + "\n";
    out += "pool_capacity " + std::to_string(db->pool()->capacity()) + "\n";
    out += "pool_resident " + std::to_string(db->pool()->size()) + "\n";
    out += "pool_pinned " + std::to_string(db->pool()->pinned_frames()) + "\n";
    out += "cache_hits " + std::to_string(db->pool()->cache_hits()) + "\n";
    out += "cache_misses " + std::to_string(db->pool()->cache_misses()) + "\n";
    std::ostringstream latency;
    if (g_last_query_latency_ms >= 0.0)
      latency << std::fixed << std::setprecision(3) << g_last_query_latency_ms;
    else
      latency << "n/a";
    out += "query_latency_ms " + latency.str() + "\n";
    out += "adaptation_enabled " +
           std::string(g_adapt_status.enabled ? "yes" : "no") + "\n";
    if (g_adapt_status.enabled) {
      out += "adaptation_container " + g_adaptation.container + "\n";
      out += "adaptation_available " +
             std::string(g_adapt_status.available ? "yes" : "no") + "\n";
      out += "adaptation_action " +
             std::string(nexus_adapt_action_str(g_adapt_status.action)) + "\n";
      out += "adaptation_reason " +
             std::string(nexus_adapt_reason_str(g_adapt_status.reason)) + "\n";
      out += "adaptation_target " +
             std::to_string(g_adapt_status.target_frames) + "\n";
      std::ostringstream pressure;
      pressure << std::fixed << std::setprecision(3) << g_adapt_status.pressure;
      out += "adaptation_pressure " + pressure.str() + "\n";
      out += "adaptation_applied " +
             std::to_string(g_adapt_status.applications) + "\n";
    }
    return out;
  }

  if (a[0] == "table") {
    if (a.size() < 2)
      return "ERR 3 usage: table <create|list> ...\n";
    if (a[1] == "create") {
      if (a.size() != 3)
        return "ERR 3 usage: table create <name>\n";
      const Status s = db->executor().CreateTable(a[2]);
      if (!s.ok())
        return Err(s);
      return QueryOk("created table " + a[2], started);
    }
    if (a[1] == "list") {
      if (a.size() != 2)
        return "ERR 3 usage: table list\n";
      std::vector<std::string> tables;
      const Status s = db->executor().ListTables(&tables);
      if (!s.ok())
        return Err(s);
      std::string out;
      for (const std::string& name : tables)
        out += name + "\n";
      return QueryLines(out, started);
    }
    return "ERR 3 unknown table subcommand: " + a[1] + "\n";
  }

  if (a[0] == "insert") {
    if (a.size() != 3)
      return "ERR 3 usage: insert <table> <hex-data>\n";
    std::vector<std::uint8_t> data;
    if (!HexDecode(a[2], &data))
      return "ERR 3 malformed hex data\n";
    QueryExecutor::RecordId id;
    const Status s = db->executor().Insert(a[1], data.data(), data.size(), &id);
    if (!s.ok())
      return Err(s);
    return QueryOk("page=" + std::to_string(id.page) +
                       " slot=" + std::to_string(id.slot),
                   started);
  }

  if (a[0] == "get" || a[0] == "update" || a[0] == "delete") {
    const std::size_t expected = (a[0] == "update") ? 5 : 4;
    if (a.size() != expected)
      return "ERR 3 wrong argument count\n";
    std::uint64_t page = 0;
    std::uint64_t slot = 0;
    if (!ParseU64(a[2], &page) || page >= Page::kInvalidId)
      return "ERR 3 invalid page id\n";
    if (!ParseU64(a[3], &slot) || slot > 0xFFFF)
      return "ERR 3 invalid slot id\n";
    QueryExecutor::RecordId id;
    id.page = static_cast<Page::PageId>(page);
    id.slot = static_cast<std::uint16_t>(slot);

    if (a[0] == "get") {
      const std::uint8_t* data = nullptr;
      std::size_t len = 0;
      const Status s = db->executor().Get(a[1], id, &data, &len);
      if (!s.ok())
        return Err(s);
      return QueryOk("page=" + a[2] + " slot=" + a[3] +
                         " len=" + std::to_string(len) +
                         " hex=" + HexEncode(data, len),
                     started);
    }
    if (a[0] == "delete") {
      const Status s = db->executor().Delete(a[1], id);
      if (!s.ok())
        return Err(s);
      return QueryOk("deleted page=" + a[2] + " slot=" + a[3], started);
    }
    std::vector<std::uint8_t> data;
    if (!HexDecode(a[4], &data))
      return "ERR 3 malformed hex data\n";
    const Status s = db->executor().Update(a[1], id, data.data(), data.size());
    if (!s.ok())
      return Err(s);
    return QueryOk("updated page=" + a[2] + " slot=" + a[3], started);
  }

  if (a[0] == "scan") {
    if (a.size() != 2)
      return "ERR 3 usage: scan <table>\n";
    std::string records;
    const Status s = db->executor().Scan(a[1], AppendScan, &records);
    if (!s.ok())
      return Err(s);
    return QueryLines(records, started);
  }

  return "ERR 3 unknown command: " + a[0] + "\n";
}

// Creates a listening Unix-domain socket at `path` (a stale path is unlinked
// first). Returns the listening fd, or -1 with *failed_stage naming the exact
// syscall that failed and *saved holding that syscall's (or the path's) errno,
// so the caller can print a precise cause instead of a bare warning.
int CreateControlSocket(const std::string& path, const char** failed_stage,
                        int* saved) {
  auto fail = [&](const char* stage, int e) {
    if (failed_stage != nullptr)
      *failed_stage = stage;
    if (saved != nullptr)
      *saved = e;
    return -1;
  };
  if (path.size() >= sizeof(sockaddr_un::sun_path))
    return fail("path", ENAMETOOLONG);
  ::unlink(path.c_str());
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return fail("socket", errno);
  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int e = errno;
    ::close(fd);
    return fail("bind", e);
  }
  if (::listen(fd, 8) != 0) {
    const int e = errno;
    ::close(fd);
    ::unlink(path.c_str());
    return fail("listen", e);
  }
  {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  // The control socket is a local, same-user channel: never leave it group- or
  // world-accessible regardless of the process umask.
  (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
  return fd;
}

// FNV-1a 64 over the absolute data-dir path: a stable, filesystem-safe key so
// sockets relocated out of a data directory remain unique per database and are
// recomputed identically by every engine/client process.
std::string SocketKeyHash(const std::string& absolute_data_dir) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char c : absolute_data_dir) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= 0x100000001b3ULL;
  }
  static const char* kHex = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = kHex[hash & 0xF];
    hash >>= 4;
  }
  return out;
}

// Directory that reliably hosts Unix-domain sockets even when the data
// directory's filesystem does not. Precedence: $NEXUSDB_SOCKET_DIR override,
// the per-user XDG_RUNTIME_DIR, then /tmp/nexusdb-<uid>.
std::string FallbackSocketDir() {
  const char* dir = std::getenv("NEXUSDB_SOCKET_DIR");
  if (dir != nullptr && dir[0] != '\0')
    return std::string(dir);
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg != nullptr && xdg[0] != '\0')
    return std::string(xdg) + "/nexusdb";
  return "/tmp/nexusdb-" + std::to_string(static_cast<long>(::getuid()));
}

// Records the actual socket path (single line) in the data directory so
// clients discover a relocated socket without duplicating fallback logic.
bool WriteSocketMarker(const std::string& data_dir,
                       const std::string& socket_path) {
  const std::string marker_path =
      (std::filesystem::path(data_dir) / "nexusdb.sock.path").string();
  std::FILE* out = std::fopen(marker_path.c_str(), "w");
  if (out == nullptr)
    return false;
  const std::string line = socket_path + "\n";
  const bool ok = std::fwrite(line.data(), 1, line.size(), out) == line.size() &&
                  std::fclose(out) == 0;
  if (!ok)
    (void)std::remove(marker_path.c_str());
  return ok;
}

bool ReadRequest(int conn, std::string* line) {
  line->clear();
  char buf[256];
  for (;;) {
    const ssize_t n = ::read(conn, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return !line->empty();
    line->append(buf, static_cast<std::size_t>(n));
    const std::size_t nl = line->find('\n');
    if (nl != std::string::npos) {
      line->erase(nl);
      return true;
    }
    if (line->size() > kMaxRequest)
      return false;
  }
}

void WriteAll(int fd, const std::string& data) {
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n =
        ::send(fd, data.data() + off, data.size() - off, kSendFlags);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    off += static_cast<std::size_t>(n);
  }
}

// Serves control requests until g_stop is set. Never spins: poll() blocks with a
// timeout and returns early on a signal so the stop flag is re-checked promptly.
void ServeControl(int listen_fd, nexusdb::Database* db) {
  while (!g_stop) {
    pollfd p;
    p.fd = listen_fd;
    p.events = POLLIN;
    p.revents = 0;
    const int ready = ::poll(&p, 1, 200);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ready == 0)
      continue;
    const int conn = ::accept(listen_fd, nullptr, nullptr);
    if (conn < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      ::usleep(50000);
      continue;
    }
    {
      timeval tv;
      tv.tv_sec = 5;
      tv.tv_usec = 0;
      (void)::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      (void)::setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
      const int no_sigpipe = 1;
      (void)::setsockopt(conn, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                         sizeof(no_sigpipe));
#endif
    }
    std::string line;
    if (ReadRequest(conn, &line)) {
      bool quit = false;
      std::string response;
      {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        response = DispatchCommand(db, line, &quit);
      }
      WriteAll(conn, response);
      if (quit)
        g_stop = 1;
    }
    ::close(conn);
  }
}

}  // namespace
#endif  // !_WIN32

int main(int argc, char** argv) {
  std::string data_dir;
  std::string socket_override;
  std::size_t pool_frames = 64;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data-dir") {
      if (i + 1 >= argc) {
        Usage(argv[0]);
        return kExitUsage;
      }
      data_dir = argv[++i];
    } else if (arg == "--buffer-pool") {
      if (i + 1 >= argc) {
        Usage(argv[0]);
        return kExitUsage;
      }
      std::size_t frames = 0;
      if (!ParseCount(argv[++i], &frames) || frames == 0) {
        Usage(argv[0]);
        return kExitUsage;
      }
      pool_frames = frames;
    } else if (arg == "--socket") {
      if (i + 1 >= argc) {
        Usage(argv[0]);
        return kExitUsage;
      }
      socket_override = argv[++i];
    } else {
      Usage(argv[0]);
      return kExitUsage;
    }
  }

  if (data_dir.empty()) {
    const char* env = std::getenv("NEXUSDB_DATA_DIR");
    data_dir = (env != nullptr && env[0] != '\0') ? env : "./nexusdb_data";
  }

  // Resolve to an absolute path once, so the database files, the control
  // socket, and its relocation marker are unambiguous and the engine and the
  // Manager always agree on the same physical directory even if the working
  // directory changes afterwards.
  {
    std::error_code abs_ec;
    const std::filesystem::path abs_dir = std::filesystem::absolute(data_dir, abs_ec);
    if (!abs_ec && !abs_dir.empty())
      data_dir = abs_dir.lexically_normal().string();
  }

  // 1. Data directory (recursive create). Use the non-throwing overloads so a
  // filesystem failure is a clean exit code, never an escaping exception.
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(data_dir, dir_ec)) {
    std::fprintf(stderr, "db_engine: cannot use data directory '%s': %s\n",
                 data_dir.c_str(),
                 dir_ec ? dir_ec.message().c_str() : ec.message().c_str());
    return kExitStorage;
  }

  const std::filesystem::path db_file =
      std::filesystem::path(data_dir) / "nexusdb.db";
  const std::filesystem::path wal_file =
      std::filesystem::path(data_dir) / "nexusdb.wal";

  std::string socket_path;
  if (socket_override != "none") {
    socket_path = socket_override.empty()
                      ? (std::filesystem::path(data_dir) / "nexusdb.sock").string()
                      : socket_override;
  }

  // 2. Open the database: PageStorage + WAL, run recovery, then wire the
  // BufferPool/M catalog/QueryExecutor. This is the same Open path the CLI and
  // tests use, so a container-hosted database has the WAL-before-data
  // guarantee and crash recovery, not just a raw storage stack.
  std::unique_ptr<nexusdb::Database> db;
  nexusdb::Status s =
      nexusdb::Database::Open(db_file.string(), wal_file.string(), pool_frames,
                              &db);
  if (!s.ok()) {
    std::fprintf(stderr, "db_engine: database open failed: %s\n",
                 s.message.c_str());
    const bool pool_error = s.code == nexusdb::DbError::kOutOfMemory ||
                            s.code == nexusdb::DbError::kBufferFull;
    return pool_error ? kExitPool : kExitStorage;
  }

  // 3. Report the recovered page count (bootstrap page 0 + catalog already
  // exist on a fresh database, created by Database::Open).
  std::uint32_t pages = 0;
  if (!db->storage()->NumPages(&pages).ok()) {
    std::fprintf(stderr, "db_engine: cannot read page count\n");
    (void)db->Close();
    return kExitStorage;
  }

  (void)std::signal(SIGINT, NexusEngineOnSignal);
  (void)std::signal(SIGTERM, NexusEngineOnSignal);

  // 4. Publish the control socket BEFORE the ready line so a client that reacts
  // to readiness can never race the bind. A socket that cannot be created is
  // fatal: an engine that stays alive without it would look RUNNING to the
  // manager while serving no control requests.
#ifndef _WIN32
  int listen_fd = -1;
  std::filesystem::path socket_marker;
  if (!socket_path.empty()) {
    socket_marker = std::filesystem::path(data_dir) / "nexusdb.sock.path";
    std::error_code rm_ec;
    (void)std::filesystem::remove(socket_marker, rm_ec);  // never serve a stale marker

    const char* failed_stage = nullptr;
    int sock_errno = 0;
    listen_fd = CreateControlSocket(socket_path, &failed_stage, &sock_errno);

    // Some filesystems cannot host Unix-domain sockets at all (bind fails
    // with EOPNOTSUPP, e.g. WSL2's drvfs/9p under /mnt/*). Relocate the
    // default (never an explicit --socket) into a runtime directory and
    // record the real location for clients.
    if (listen_fd < 0 && socket_override.empty() &&
        (sock_errno == EOPNOTSUPP || sock_errno == EPROTONOSUPPORT)) {
      const std::string fallback_dir = FallbackSocketDir();
      std::error_code dir_ec;
      std::filesystem::create_directories(fallback_dir, dir_ec);
      (void)::chmod(fallback_dir.c_str(), S_IRWXU);
      const std::filesystem::path fallback_path =
          std::filesystem::path(fallback_dir) /
          ("nexusdb-" + SocketKeyHash(data_dir) + ".sock");
      std::fprintf(stderr,
                   "db_engine: control socket at %s: %s failed: %s "
                   "(errno=%d); filesystem cannot host Unix sockets, "
                   "relocating to %s\n",
                   socket_path.c_str(), failed_stage, std::strerror(sock_errno),
                   sock_errno, fallback_path.string().c_str());
      listen_fd =
          CreateControlSocket(fallback_path.string(), &failed_stage, &sock_errno);
      if (listen_fd >= 0) {
        if (WriteSocketMarker(data_dir, fallback_path.string())) {
          socket_path = fallback_path.string();
        } else {
          // Clients locate the socket only through the marker: without it the
          // engine would serve requests nobody can address. Fail cleanly.
          failed_stage = "marker-write";
          sock_errno = errno;
          ::close(listen_fd);
          ::unlink(fallback_path.c_str());
          listen_fd = -1;
        }
      }
    }

    if (listen_fd < 0) {
      std::fprintf(stderr,
                   "db_engine: control socket unavailable at %s: %s failed: "
                   "%s (errno=%d)\n",
                   socket_path.c_str(), failed_stage, std::strerror(sock_errno),
                   sock_errno);
      (void)db->Close();
      return kExitSocket;
    }
  }
#else
  (void)socket_path;
#endif

  std::thread adaptation_thread;
  const std::string container = ContainerNameFromDataDir(data_dir);
  if (!container.empty()) {
    const std::string cgroup_dir = CgroupDirFor(container);
    if (!cgroup_dir.empty()) {
      g_adaptation.container = container;
      g_adaptation.cgroup_dir = cgroup_dir;
      nexus_policy_config_default(&g_adaptation.config);
      g_adaptation.config.page_size =
          static_cast<unsigned long>(nexusdb::kPageSize);
      g_adaptation.config.max_frames =
          static_cast<unsigned long>(kMaxPoolFrames);
      nexus_policy_init(&g_adaptation.policy);
      g_adaptation.enabled = true;
      g_adapt_status.enabled = true;
      g_adapt_status.target_frames = db->pool()->capacity();
      adaptation_thread = std::thread(RunAdaptation, db.get());
    }
  }

  std::printf("db_engine: ready pid=%ld data=%s pages=%u pool=%zu",
              CurrentPid(), db_file.string().c_str(), pages, pool_frames);
#ifndef _WIN32
  if (listen_fd >= 0)  // a failed socket exits above; never claim a socket we do not serve
#else
  if (!socket_path.empty())
#endif
    std::printf(" socket=%s", socket_path.c_str());
  std::printf("\n");
  std::fflush(stdout);

  // 5. Serve control requests (or idle) until a shutdown request. No busy loop.
#ifndef _WIN32
  if (listen_fd >= 0)
    ServeControl(listen_fd, db.get());
  else
    while (!g_stop)
      IdleOnce();
  if (listen_fd >= 0) {
    ::close(listen_fd);
    ::unlink(socket_path.c_str());
    std::error_code rm_ec;
    (void)std::filesystem::remove(socket_marker, rm_ec);
  }
#else
  while (!g_stop)
    IdleOnce();
#endif

  g_stop = 1;
  if (adaptation_thread.joinable())
    adaptation_thread.join();

  // 6. Graceful shutdown: flush all dirty pages (WAL-ordered), checkpoint and
  // close the log, then close storage.
  const nexusdb::Status close_status = db->Close();
  if (!close_status.ok()) {
    std::fprintf(stderr, "db_engine: shutdown flush failed: %s\n",
                 close_status.message.c_str());
    return kExitShutdown;
  }

  std::printf("db_engine: clean shutdown pid=%ld\n", CurrentPid());
  std::fflush(stdout);
  return kExitOk;
}
