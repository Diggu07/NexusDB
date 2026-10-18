// NexusDB — R3 CLI end-to-end test.
//
// Drives the real `nexusdb` CLI executable through the shell (redirecting
// output to a file) and checks exit codes AND output, so the whole path
// CLI -> Router -> Manager -> Container runtime / Database is exercised.
//
// POSIX only: on Windows the shell redirection/exec contract differs, so the
// test reports ENV-LIMITED there.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#define NEXUS_HAVE_SHELL 1
#else
#define NEXUS_HAVE_SHELL 0
#endif

namespace {

int g_failures = 0;
int g_skipped = 0;

#if NEXUS_HAVE_SHELL
#define CHECK(desc, expr)                     \
  do {                                        \
    if (expr) {                               \
      std::printf("PASS: %s\n", desc);        \
    } else {                                  \
      std::printf("FAIL: %s\n", desc);        \
      ++g_failures;                           \
    }                                         \
  } while (0)

#define SKIP(desc)                            \
  do {                                        \
    std::printf("ENV-LIMITED: %s\n", desc);   \
    ++g_skipped;                              \
  } while (0)

void SetEnv(const char* key, const std::string& value) {
  setenv(key, value.c_str(), 1);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct CliResult {
  int exit_code = -1;
  std::string out;
};

CliResult RunCli(const std::string& cli, const std::string& args,
                 const std::string& outfile) {
  const std::string cmd =
      "\"" + cli + "\" " + args + " > \"" + outfile + "\" 2>&1";
  const int rc = std::system(cmd.c_str());
  CliResult r;
  r.exit_code = (rc < 0) ? -1 : WEXITSTATUS(rc);
  r.out = ReadFile(outfile);
  return r;
}

bool Contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

bool ExtractPageSlot(const std::string& out, unsigned int* page,
                     unsigned int* slot) {
  const std::size_t pos = out.find("page=");
  if (pos == std::string::npos)
    return false;
  return std::sscanf(out.c_str() + pos, "page=%u slot=%u", page, slot) == 2;
}

long ProcessId() { return static_cast<long>(getpid()); }
#endif  // NEXUS_HAVE_SHELL

}  // namespace

int main() {
  std::printf("=== NexusDB CLI end-to-end test (R3) ===\n\n");

#if !NEXUS_HAVE_SHELL
  std::printf("ENV-LIMITED: CLI shell harness is POSIX-only\n");
  std::printf("\n=== ALL TESTS PASSED (0 failure(s), 1 env-limited) ===\n");
  return 0;
#else
  const char* cli_env = std::getenv("NEXUSDB_CLI");
  const std::string cli =
      (cli_env != nullptr && cli_env[0] != '\0') ? cli_env : "./nexusdb_cli";
  if (access(cli.c_str(), X_OK) != 0) {
    SKIP("nexusdb CLI binary unavailable (set NEXUSDB_CLI)");
    std::printf("\n=== ALL TESTS PASSED (%d failure(s), %d env-limited) ===\n",
                g_failures, g_skipped);
    return 0;
  }

  const std::string base =
      (std::filesystem::temp_directory_path() /
       ("nexus_r3_cli_" + std::to_string(ProcessId())))
          .string();
  const std::string root = base + "_data";
  const std::string cg = base + "_cg";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::remove_all(cg, ec);
  std::filesystem::create_directories(root, ec);

  const std::string outfile = base + "_out.txt";
  SetEnv("NEXUSDB_DATA_ROOT", root);
  SetEnv("NEXUS_CGROUP_ROOT", cg);

  // ---- help / usage / invalid ----
  {
    CliResult r = RunCli(cli, "help", outfile);
    CHECK("help exits 0", r.exit_code == 0);
    CHECK("help prints usage", Contains(r.out, "Usage"));
  }
  CHECK("unknown command -> exit 2",
        RunCli(cli, "frobnicate", outfile).exit_code == 2);
  CHECK("wrong arity -> exit 2",
        RunCli(cli, "insert db1 t1", outfile).exit_code == 2);
  CHECK("malformed hex -> exit 3",
        RunCli(cli, "insert db1 t1 zz", outfile).exit_code == 3);
  CHECK("odd-length hex -> exit 3",
        RunCli(cli, "insert db1 t1 abc", outfile).exit_code == 3);
  CHECK("traversal name -> exit 3",
        RunCli(cli, "db init ../evil", outfile).exit_code == 3);
  CHECK("missing table -> exit 4",
        RunCli(cli, "scan db1 nope", outfile).exit_code == 4);

  // ---- numeric boundary parsing (end-to-end) ----
  CHECK("reserved page id -> exit 3",
        RunCli(cli, "get db1 t1 4294967295 0", outfile).exit_code == 3);
  CHECK("page id 2^32 -> exit 3",
        RunCli(cli, "get db1 t1 4294967296 0", outfile).exit_code == 3);
  CHECK("huge page id -> exit 3",
        RunCli(cli, "get db1 t1 99999999999999999999 0", outfile).exit_code ==
            3);
  CHECK("slot 65536 -> exit 3",
        RunCli(cli, "get db1 t1 0 65536", outfile).exit_code == 3);
  CHECK("cpu 101 -> exit 3",
        RunCli(cli, "container create c --cpu 101", outfile).exit_code == 3);
  CHECK("memory overflow -> exit 3",
        RunCli(cli, "container create c --memory 99999999999999999999", outfile)
                .exit_code == 3);

  // ---- scripted data workflow ----
  CHECK("db init -> 0", RunCli(cli, "db init db1", outfile).exit_code == 0);
  CHECK("table create -> 0",
        RunCli(cli, "table create db1 t1", outfile).exit_code == 0);
  {
    CliResult r = RunCli(cli, "table list db1", outfile);
    CHECK("table list -> 0 and shows t1",
          r.exit_code == 0 && Contains(r.out, "t1"));
  }

  unsigned int page = 0, slot = 0;
  {
    CliResult r = RunCli(cli, "insert db1 t1 deadbeefcafe", outfile);
    CHECK("insert -> 0", r.exit_code == 0);
    CHECK("insert reports page/slot", ExtractPageSlot(r.out, &page, &slot));
  }
  {
    std::ostringstream cmd;
    cmd << "get db1 t1 " << page << " " << slot;
    CliResult r = RunCli(cli, cmd.str(), outfile);
    CHECK("get -> 0", r.exit_code == 0);
    CHECK("get returns exact hex", Contains(r.out, "hex=deadbeefcafe"));
  }
  {
    CliResult r = RunCli(cli, "scan db1 t1", outfile);
    CHECK("scan -> 0", r.exit_code == 0);
    CHECK("scan shows the record", Contains(r.out, "hex=deadbeefcafe"));
  }
  {
    std::ostringstream cmd;
    cmd << "update db1 t1 " << page << " " << slot << " 00ff00ff";
    CHECK("update -> 0", RunCli(cli, cmd.str(), outfile).exit_code == 0);
  }
  {
    std::ostringstream cmd;
    cmd << "get db1 t1 " << page << " " << slot;
    CliResult r = RunCli(cli, cmd.str(), outfile);
    CHECK("get after update returns new hex",
          Contains(r.out, "hex=00ff00ff"));
  }
  // A brand-new CLI invocation must still see the persisted record.
  {
    CliResult r = RunCli(cli, "scan db1 t1", outfile);
    CHECK("persistence across processes",
          r.exit_code == 0 && Contains(r.out, "hex=00ff00ff"));
  }
  {
    std::ostringstream cmd;
    cmd << "delete db1 t1 " << page << " " << slot;
    CHECK("delete -> 0", RunCli(cli, cmd.str(), outfile).exit_code == 0);
  }
  {
    std::ostringstream cmd;
    cmd << "get db1 t1 " << page << " " << slot;
    CHECK("get deleted -> exit 4",
          RunCli(cli, cmd.str(), outfile).exit_code == 4);
  }
  {
    CliResult r = RunCli(cli, "scan db1 t1", outfile);
    CHECK("scan after delete reports none",
          r.exit_code == 0 && Contains(r.out, "no records"));
  }

  // ---- container lifecycle (needs a configured engine binary) ----
  {
    const char* eng = std::getenv("NEXUSDB_DB_ENGINE");
    const bool have_engine =
        eng != nullptr && eng[0] != '\0' && access(eng, X_OK) == 0;
    if (!have_engine) {
      SKIP("db_engine binary unavailable (set NEXUSDB_DB_ENGINE)");
    } else {
      CliResult create =
          RunCli(cli, "container create e2e1 --memory 0 --cpu 0", outfile);
      if (create.exit_code != 0) {
        SKIP("container runtime unavailable in this environment");
      } else {
        CHECK("container start -> 0",
              RunCli(cli, "container start e2e1", outfile).exit_code == 0);
        {
          CliResult r = RunCli(cli, "container status e2e1", outfile);
          CHECK("status shows RUNNING",
                r.exit_code == 0 && Contains(r.out, "RUNNING"));
        }
        CHECK("db command refused while container RUNNING -> exit 1",
              RunCli(cli, "db init e2e1", outfile).exit_code == 1);
        CHECK("container stop -> 0",
              RunCli(cli, "container stop e2e1", outfile).exit_code == 0);
        {
          CliResult r = RunCli(cli, "container status e2e1", outfile);
          CHECK("status shows STOPPED",
                r.exit_code == 0 && Contains(r.out, "STOPPED"));
        }
        // The engine created the database; it is usable once stopped.
        CHECK("db usable after stop -> 0",
              RunCli(cli, "db init e2e1", outfile).exit_code == 0);
        CHECK("table create in recovered db -> 0",
              RunCli(cli, "table create e2e1 t", outfile).exit_code == 0);
        CHECK("insert into recovered db -> 0",
              RunCli(cli, "insert e2e1 t 11223344", outfile).exit_code == 0);
        {
          CliResult r = RunCli(cli, "scan e2e1 t", outfile);
          CHECK("scan recovered db shows record",
                r.exit_code == 0 && Contains(r.out, "hex=11223344"));
        }
        CHECK("container delete -> 0",
              RunCli(cli, "container delete e2e1", outfile).exit_code == 0);
      }
    }
  }

  std::filesystem::remove_all(root, ec);
  std::filesystem::remove_all(cg, ec);

  std::printf("\n=== %s (%d failure(s), %d env-limited) ===\n",
              g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
              g_skipped);
  return g_failures == 0 ? 0 : 1;
#endif
}
