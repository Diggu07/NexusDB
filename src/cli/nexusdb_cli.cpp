// NexusDB — command-line interface (Digvijay: CLI, R3).
//
// A deliberately tiny front end: it forwards argv to the Router (which parses
// and validates) and the Router dispatches to the Manager. All behaviour lives
// in the Manager/Router; this file only prints results and returns exit codes.
//
// There is no daemon and no client/server protocol. Database commands open the
// database file directly (running WAL recovery on open); container commands
// drive the container runtime. See `nexusdb help`.

#include <cstdio>
#include <string>
#include <vector>

#include "manager/manager.h"
#include "manager/router.h"

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);

  nexusdb::Manager manager;
  const nexusdb::cli::Result result = nexusdb::cli::Run(&manager, args);

  if (!result.output.empty()) {
    std::FILE* stream = (result.exit_code == nexusdb::cli::kOk) ? stdout : stderr;
    std::fputs(result.output.c_str(), stream);
    if (result.output.back() != '\n')
      std::fputc('\n', stream);
  }
  return result.exit_code;
}
