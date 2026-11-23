// NexusDB — command Router (Digvijay: Manager/Router, R3).
//
// The Router is the thin, testable seam between the CLI text and the Manager.
// It owns ONLY argument syntax and input validation (arg counts, numeric
// parsing, hexadecimal decoding); all domain behaviour lives in the Manager.
//
//   argv -> ParseArgs -> Command -> Dispatch(Manager) -> Result{exit_code, output}
//
// Exit codes are stable and documented (CliExit below). The CLI prints
// Result.output and returns Result.exit_code.
//
// Binary record payloads are passed as an even-length hexadecimal string
// (e.g. "deadbeef"); an odd length or a non-hex character is an invalid
// argument (exit 3). No exception escapes: parsing returns a code, dispatch
// returns a Status.

#ifndef NEXUSDB_MANAGER_ROUTER_H
#define NEXUSDB_MANAGER_ROUTER_H

#include <cstdint>
#include <string>
#include <vector>

#include "common/error.h"
#include "db/page.h"
#include "manager/manager.h"

namespace nexusdb {
namespace cli {

// Stable CLI exit codes.
enum CliExit {
  kOk = 0,        // success
  kError = 1,     // operational failure (I/O, state, pool, runtime)
  kUsage = 2,     // unknown command, wrong argument count, bad flag
  kInvalid = 3,   // bad value: invalid name, malformed hex, bad number
  kNotFound = 4,  // container/table/record does not exist
};

enum class CommandKind {
  kHelp,
  kContainerCreate,
  kContainerStart,
  kContainerStop,
  kContainerStatus,
  kContainerDelete,
  kContainerList,
  kContainerExec,
  kDbInit,
  kTableCreate,
  kTableList,
  kInsert,
  kGet,
  kUpdate,
  kDelete,
  kScan,
};

struct Command {
  CommandKind kind = CommandKind::kHelp;
  std::string name;     // container / database name
  std::string table;
  std::vector<std::uint8_t> data;
  Page::PageId page = 0;
  std::uint16_t slot = 0;
#ifdef _WIN32
  unsigned long long memory = 0;
#else
  unsigned long memory = 0;
#endif
  unsigned int cpu = 0;
  std::string engine;
  std::string request;      // raw request text for `exec`
  std::string program_path; // argv[0], used to locate db_engine by default
  std::string help_topic;   // "" or the subcommand name for `<cmd> --help`
  bool engine_explicit = false; // --engine or --no-engine was provided
};

struct Result {
  int exit_code = kOk;
  std::string output;
};

// Parses argv (excluding the program name) into `out`. Returns kOk, kUsage, or
// kInvalid; on non-ok `*error` receives a human-readable message.
int ParseArgs(const std::vector<std::string>& args, Command* out,
              std::string* error);

// Dispatches an already-parsed command to the manager.
Result Dispatch(Manager* mgr, const Command& cmd);

// Parse + dispatch in one call (what main() uses). `program_path` is argv[0];
// it is used to resolve the default `db_engine` executable that sits next to
// the CLI. Passing an empty path keeps the previous behavior (no default).
Result Run(Manager* mgr, const std::vector<std::string>& args,
           const std::string& program_path = std::string());

// Usage/help text. HelpText() is the overview; HelpTopic(name) returns focused
// help for a command (e.g. "create", "status") or the overview when unknown.
const char* HelpText();
const char* HelpTopic(const std::string& name);

}  // namespace cli
}  // namespace nexusdb

#endif  // NEXUSDB_MANAGER_ROUTER_H
