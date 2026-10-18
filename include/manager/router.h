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
  std::string name;   // container / database name
  std::string table;
  std::vector<std::uint8_t> data;
  Page::PageId page = 0;
  std::uint16_t slot = 0;
  unsigned long memory = 0;
  unsigned int cpu = 0;
  std::string engine;
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

// Parse + dispatch in one call (what main() uses).
Result Run(Manager* mgr, const std::vector<std::string>& args);

// Usage/help text (also returned by the `help` command).
const char* HelpText();

}  // namespace cli
}  // namespace nexusdb

#endif  // NEXUSDB_MANAGER_ROUTER_H
