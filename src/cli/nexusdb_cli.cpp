// NexusDB — command-line interface (Digvijay: CLI, R3).
//
// A deliberately tiny front end: it forwards argv to the Router (which parses
// and validates) and the Router dispatches to the Manager. All behaviour lives
// in the Manager/Router; this file only prints results and returns exit codes.
//
// Database commands open the database file directly when the container is
// stopped (running WAL recovery on open). When the container is RUNNING, the
// Router proxies the command to the engine's Unix-domain control socket so it
// never races the engine's page writes. See `nexusdb help`.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "container/cgroup.h"
#include "manager/manager.h"
#include "manager/router.h"

namespace {

// Absolute path of this executable, used to find a sibling `db_engine` so
// `nexusdb create` works without --engine from any working directory.
std::string SelfPath(const char* argv0) {
#ifdef _WIN32
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
    return std::string(buf, n);
#else
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return std::string(buf);
  }
#endif
  return argv0 != nullptr ? std::string(argv0) : std::string();
}

std::string TextToHex(const std::string& text) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() * 2);
  for (unsigned char byte : text) {
    out.push_back(kHex[(byte >> 4) & 0xF]);
    out.push_back(kHex[byte & 0xF]);
  }
  return out;
}

std::size_t RecordPayloadIndex(const std::vector<std::string>& args) {
  if (args.empty())
    return 0;
  if (args[0] == "insert")
    return 3;
  if (args[0] == "update")
    return 5;
  return 0;
}

bool PrepareRecordArgs(std::vector<std::string>* args, std::string* error) {
  const std::size_t index = RecordPayloadIndex(*args);
  if (index == 0 || args->size() <= index)
    return true;
  if ((*args)[index] != "--hex") {
    (*args)[index] = TextToHex((*args)[index]);
    return true;
  }
  if (args->size() == index + 1) {
    *error = args->front() == "insert"
                 ? "usage: nexusdb insert <name> <table> --hex <hex-data>"
                 : "usage: nexusdb update <name> <table> <page-id> <slot-id> "
                   "--hex <hex-data>";
    return false;
  }
  args->erase(args->begin() +
              static_cast<std::vector<std::string>::difference_type>(index));
  return true;
}

#ifndef _WIN32
// Only `start` (and `container start`) needs to migrate the freshly spawned
// workload into the delegated cgroup. create/status/exec/stop/delete operate on
// files or already-running processes and need no re-exec.
bool IsStartCommand(const std::vector<std::string>& args) {
  if (args.empty())
    return false;
  if (args[0] == "start")
    return args.size() >= 2 && args[1] != "--help" && args[1] != "-h";
  if (args[0] == "container" && args.size() >= 3 && args[1] == "start")
    return args[2] != "--help" && args[2] != "-h";
  return false;
}

// WSL starts interactive shells outside the systemd user cgroup subtree
// (/non-systemd or a root-owned login session scope), so a child cannot be
// migrated into the user-delegated cgroup. Re-exec just the start command in a
// transient user scope: systemd-run is the supported, least-privilege way to
// enter that subtree. Real kernel limits remain enforced either way.
void EnterDelegatedScopeOrExit(const std::string& self,
                               const std::vector<std::string>& args) {
  const char* attempted = std::getenv("NEXUSDB_DELEGATION_ATTEMPTED");
  if ((attempted != nullptr && std::strcmp(attempted, "1") == 0) ||
      cgroup_self_in_delegated_root())
    return;

  std::vector<std::string> command;
  command.emplace_back("systemd-run");
  command.emplace_back("--user");
  command.emplace_back("--scope");
  command.emplace_back("--quiet");
  command.emplace_back("--setenv=NEXUSDB_DELEGATION_ATTEMPTED=1");

  // systemd-run does not automatically inherit the caller's shell environment;
  // forward only the documented NexusDB hooks (plus the CLI path used by tests).
  const char* forwarded[] = {"NEXUSDB_DATA_ROOT", "NEXUSDB_DB_ENGINE",
                             "NEXUS_CGROUP_ROOT", "NEXUSDB_CLI",
                             "NEXUSDB_SOCKET_DIR"};
  for (const char* key : forwarded) {
    const char* value = std::getenv(key);
    if (value != nullptr && value[0] != '\0')
      command.emplace_back(std::string("--setenv=") + key + "=" + value);
  }

  command.emplace_back("--");
  command.emplace_back(self);
  command.insert(command.end(), args.begin(), args.end());

  std::vector<char*> argv;
  argv.reserve(command.size() + 1);
  for (std::string& arg : command)
    argv.push_back(arg.data());
  argv.push_back(nullptr);

  ::execvp("systemd-run", argv.data());

  const int saved_errno = errno;
  std::fprintf(stderr,
               "nexusdb: cannot enter the delegated cgroup subtree via "
               "systemd-run: %s\n",
               std::strerror(saved_errno));
  std::fprintf(stderr,
               "nexusdb: run '%s' inside a cgroup you own, or launch it with:\n"
               "  systemd-run --user --scope -- %s ...\n",
               self.c_str(), self.c_str());
  std::exit(127);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);

  std::string arg_error;
  if (!PrepareRecordArgs(&args, &arg_error)) {
    std::fprintf(stderr, "%s\n", arg_error.c_str());
    return nexusdb::cli::kUsage;
  }

  const std::string self = SelfPath(argc > 0 ? argv[0] : nullptr);
#ifndef _WIN32
  if (IsStartCommand(args))
    EnterDelegatedScopeOrExit(self, args);
#endif

  nexusdb::Manager manager;
  const nexusdb::cli::Result result =
      nexusdb::cli::Run(&manager, args, self);

  if (!result.output.empty()) {
    std::FILE* stream = (result.exit_code == nexusdb::cli::kOk) ? stdout : stderr;
    std::fputs(result.output.c_str(), stream);
    if (result.output.back() != '\n')
      std::fputc('\n', stream);
  }
  return result.exit_code;
}
