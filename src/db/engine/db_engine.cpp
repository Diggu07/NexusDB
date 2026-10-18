// NexusDB — minimal long-running database-engine executable (P3).
//
// This is the executable the Container Engine launches as a container's
// workload. It is deliberately minimal and honest: there is NO SQL parser,
// NO network listener, and NO client/server protocol. It opens the full
// Database (PageStorage + WAL + BufferPool + Catalog/QueryExecutor), running
// WAL recovery on startup, keeps the database open, and stays alive until the
// container asks it to stop, at which point it flushes (WAL-ordered) and
// closes cleanly. An abrupt termination leaves the WAL for the next Open.
//
// Contract
// --------
//   Usage:
//     db_engine [--data-dir <dir>] [--buffer-pool <frames>]
//
//     --data-dir <dir>     directory holding the database file `nexusdb.db`
//                          (created if missing). Default: $NEXUSDB_DATA_DIR if
//                          set, else ./nexusdb_data.
//     --buffer-pool <n>    buffer-pool capacity in pages (default 64, min 1).
//
//   Startup:
//     1. parse arguments (unknown/missing-value -> usage error);
//     2. create the data directory (recursively);
//     3. Database::Open(<dir>/nexusdb.db, <dir>/nexusdb.wal, pool) -- this
//        opens the log, replays it into storage BEFORE the pool exists, then
//        checkpoints and opens the Catalog/QueryExecutor; on a fresh database
//        it reserves header page 0 and creates a catalog at page 1;
//     4. print a ready line and block until a shutdown request.
//
//   Shutdown (SIGTERM / SIGINT): Database::Close() flushAlls the pool in
//   WAL order, resets/closes the log, and closes the storage, then exits 0. The
//   process does NOT busy-loop: it blocks in pause() (POSIX) / Sleep(1000)
//   (Windows) between checks of the stop flag.
//
//   Exit codes:
//     0  clean shutdown
//     2  usage / argument error
//     3  storage open / initialization failure
//     4  buffer-pool creation failure (or a pool operation failed at startup)
//     5  flush / close failure during shutdown
//
// Platform: builds on Linux/WSL2 (the real target) and MinGW/Windows. A
// windowless Windows child cannot receive SIGTERM/SIGINT, so the container
// terminates it (TerminateProcess); the graceful flush path is the POSIX one.
// The Container Engine only launches a configured db_engine on POSIX (Windows
// returns NEXUS_ERR_UNSUPPORTED); standalone, the binary still runs on Windows.

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common/error.h"
#include "db/database.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitStorage = 3;
constexpr int kExitPool = 4;
constexpr int kExitShutdown = 5;

// Largest accepted --buffer-pool value, so a typo cannot request an absurd
// allocation. Well above any realistic small database.
constexpr std::size_t kMaxPoolFrames = 1u << 20;

void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [--data-dir <dir>] [--buffer-pool <frames>]\n"
               "  --data-dir <dir>    directory holding nexusdb.db "
               "(default: $NEXUSDB_DATA_DIR or ./nexusdb_data)\n"
               "  --buffer-pool <n>   buffer-pool capacity in pages "
               "(default: 64)\n",
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

// Blocks until a signal arrives (POSIX) or for one second (Windows). Never
// spins the CPU; the caller re-checks the stop flag after each return.
void IdleOnce() {
#ifdef _WIN32
  Sleep(1000);
#else
  (void)pause();
#endif
}

}  // namespace

// Stop flag + handler. The handler must be async-signal-safe: it only sets a
// sig_atomic_t; all flushing happens after the idle loop returns.
namespace {
volatile std::sig_atomic_t g_stop = 0;
}  // namespace

extern "C" void NexusEngineOnSignal(int sig) {
  (void)sig;
  g_stop = 1;
}

int main(int argc, char** argv) {
  std::string data_dir;
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
    } else {
      Usage(argv[0]);
      return kExitUsage;
    }
  }

  if (data_dir.empty()) {
    const char* env = std::getenv("NEXUSDB_DATA_DIR");
    data_dir = (env != nullptr && env[0] != '\0') ? env : "./nexusdb_data";
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

  std::printf("db_engine: ready pid=%ld data=%s pages=%u pool=%zu\n",
              CurrentPid(), db_file.string().c_str(), pages, pool_frames);
  std::fflush(stdout);

  // 4. Idle until a shutdown request. No busy loop.
  while (!g_stop)
    IdleOnce();

  // 5. Graceful shutdown: flush all dirty pages (WAL-ordered), checkpoint and
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
