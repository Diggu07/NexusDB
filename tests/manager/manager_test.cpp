// NexusDB — R3 Manager/Router test.
//
// Exercises the Manager (container-metadata persistence + database CRUD over
// Database/QueryExecutor) and the Router (argument syntax + validation) against
// an isolated temporary data directory. Verifies returned record BYTES and
// persisted state, not just exit codes.
//
// Same plain PASS/FAIL style as the other DB tests: exit 0 on success.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/error.h"
#include "db/page.h"
#include "manager/manager.h"
#include "manager/router.h"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#elif defined(_WIN32)
#include <process.h>
#endif

namespace {

using nexusdb::DbError;
using nexusdb::Manager;
using nexusdb::Page;
using nexusdb::Status;
using nexusdb::cli::Command;
using nexusdb::cli::CommandKind;
using nexusdb::cli::ParseArgs;
using nexusdb::cli::Run;

int g_failures = 0;

#define CHECK(desc, expr)                     \
  do {                                        \
    if (expr) {                               \
      std::printf("PASS: %s\n", desc);        \
    } else {                                  \
      std::printf("FAIL: %s\n", desc);        \
      ++g_failures;                           \
    }                                         \
  } while (0)

long ProcessId() {
#if defined(__unix__) || defined(__APPLE__)
  return static_cast<long>(getpid());
#elif defined(_WIN32)
  return static_cast<long>(_getpid());
#else
  return 0;
#endif
}

void SetEnv(const char* key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

void RemoveAll(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

bool Contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

std::string MakeBytes(std::initializer_list<std::uint8_t> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.begin()), bytes.size());
}

void WriteRunningMeta(const std::string& root, const std::string& name,
                      int state) {
  const std::string dir = root + "/.nexus/containers";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(dir + "/" + name + ".meta", std::ios::trunc);
  out << "name=" << name << "\n"
      << "id=nx-000001\n"
      << "memory=0\n"
      << "cpu=0\n"
      << "engine=\n"
      << "cgroup=" << root << "_cg/" << name << "\n"
      << "pid=999999\n"
      << "state=" << state << "\n";
}

}  // namespace

int main() {
  const std::string base =
      (std::filesystem::temp_directory_path() /
       ("nexus_r3_manager_" + std::to_string(ProcessId())))
          .string();
  const std::string root = base + "_data";
  const std::string cg = base + "_cg";
  RemoveAll(root);
  RemoveAll(cg);

  // Isolate the container runtime's cgroup root and the manager's default data
  // root so nothing touches the real system.
  SetEnv("NEXUS_CGROUP_ROOT", cg);
  SetEnv("NEXUSDB_DATA_ROOT", root);

  Manager mgr(root);
  CHECK("Manager::Init", mgr.Init().ok());

  // ---- identifier validation ----
  CHECK("valid name accepted", Manager::IsValidName("db1"));
  CHECK("traversal name rejected", !Manager::IsValidName("../evil"));
  CHECK("separator name rejected", !Manager::IsValidName("a/b"));
  CHECK("empty name rejected", !Manager::IsValidName(""));
  CHECK("db init with traversal name -> invalid",
        mgr.DbInit("../evil").code == DbError::kInvalidArgument);

  // ---- db init + reopen (idempotent) ----
  CHECK("db init", mgr.DbInit("db1").ok());
  CHECK("database file created",
        std::filesystem::exists(root + "/db1/nexusdb.db"));
  CHECK("db init is idempotent", mgr.DbInit("db1").ok());

  // ---- table create / list ----
  CHECK("create table", mgr.TableCreate("db1", "t1").ok());
  CHECK("duplicate table -> invalid",
        mgr.TableCreate("db1", "t1").code == DbError::kInvalidArgument);
  CHECK("invalid table name -> invalid",
        mgr.TableCreate("db1", "bad name").code == DbError::kInvalidArgument);
  std::vector<std::string> tables;
  CHECK("list tables", mgr.TableList("db1", &tables).ok());
  CHECK("one table listed", tables.size() == 1 && tables[0] == "t1");

  // ---- insert / get (bytes verified) ----
  const std::string rec1 = MakeBytes({0xde, 0xad, 0xbe, 0xef, 0x00, 0x11});
  Page::PageId p1 = Page::kInvalidId;
  std::uint16_t s1 = 0xFFFF;
  CHECK("insert record",
        mgr.Insert("db1", "t1",
                   reinterpret_cast<const std::uint8_t*>(rec1.data()),
                   rec1.size(), &p1, &s1)
            .ok());
  CHECK("insert returns valid id", p1 != Page::kInvalidId && s1 != 0xFFFF);

  std::vector<std::uint8_t> got;
  CHECK("get record", mgr.Get("db1", "t1", p1, s1, &got).ok());
  CHECK("get returns exact bytes", got.size() == rec1.size() &&
                                       std::memcmp(got.data(), rec1.data(),
                                                   rec1.size()) == 0);

  // ---- update (bytes verified) ----
  const std::string rec1b = MakeBytes({0x00, 0xff, 0x00, 0xff});
  CHECK("update record",
        mgr.Update("db1", "t1", p1, s1,
                   reinterpret_cast<const std::uint8_t*>(rec1b.data()),
                   rec1b.size())
            .ok());
  got.clear();
  CHECK("get after update", mgr.Get("db1", "t1", p1, s1, &got).ok());
  CHECK("updated bytes returned", got.size() == rec1b.size() &&
                                      std::memcmp(got.data(), rec1b.data(),
                                                  rec1b.size()) == 0);

  // ---- second record + scan ordering/content ----
  const std::string rec2 = MakeBytes({0xca, 0xfe});
  Page::PageId p2 = Page::kInvalidId;
  std::uint16_t s2 = 0xFFFF;
  CHECK("insert second record",
        mgr.Insert("db1", "t1",
                   reinterpret_cast<const std::uint8_t*>(rec2.data()),
                   rec2.size(), &p2, &s2)
            .ok());
  std::vector<nexusdb::ScanRecord> recs;
  CHECK("scan", mgr.Scan("db1", "t1", &recs).ok());
  CHECK("scan returns two records", recs.size() == 2);
  bool all_ok = recs.size() == 2;
  if (all_ok) {
    all_ok = recs[0].page == p1 && recs[0].slot == s1 &&
             recs[0].data.size() == rec1b.size() &&
             std::memcmp(recs[0].data.data(), rec1b.data(), rec1b.size()) == 0 &&
             recs[1].page == p2 && recs[1].slot == s2 &&
             recs[1].data.size() == rec2.size() &&
             std::memcmp(recs[1].data.data(), rec2.data(), rec2.size()) == 0;
  }
  CHECK("scan records match inserts/updates", all_ok);

  // ---- errors ----
  CHECK("insert into missing table -> not found",
        mgr.Insert("db1", "nope",
                   reinterpret_cast<const std::uint8_t*>(rec2.data()),
                   rec2.size(), nullptr, nullptr)
                .code == DbError::kNotFound);
  CHECK("get missing id -> not found",
        mgr.Get("db1", "t1", 9999, 0, &got).code == DbError::kNotFound);

  // ---- persistence across a fresh Manager (new process semantics) ----
  {
    Manager reopened(root);
    std::vector<nexusdb::ScanRecord> again;
    CHECK("reopen: scan succeeds", reopened.Scan("db1", "t1", &again).ok());
    CHECK("reopen: two records persisted", again.size() == 2);
    bool persisted = again.size() == 2;
    if (persisted) {
      persisted = again[0].data.size() == rec1b.size() &&
                  std::memcmp(again[0].data.data(), rec1b.data(),
                              rec1b.size()) == 0 &&
                  again[1].data.size() == rec2.size() &&
                  std::memcmp(again[1].data.data(), rec2.data(),
                              rec2.size()) == 0;
    }
    CHECK("reopen: record bytes persisted", persisted);
  }

  // ---- delete ----
  CHECK("delete record", mgr.Delete("db1", "t1", p2, s2).ok());
  CHECK("get deleted -> not found",
        mgr.Get("db1", "t1", p2, s2, &got).code == DbError::kNotFound);
  std::vector<nexusdb::ScanRecord> after_del;
  CHECK("scan after delete", mgr.Scan("db1", "t1", &after_del).ok());
  CHECK("one record remains", after_del.size() == 1);

  // ---- RUNNING guard (no cgroup needed: synthesized metadata) ----
  WriteRunningMeta(root, "run1", 1 /* RUNNING */);
  CHECK("database op refused while container RUNNING",
        mgr.DbInit("run1").code == DbError::kBadState);
  WriteRunningMeta(root, "run1", 2 /* STOPPED */);
  CHECK("database op allowed when STOPPED", mgr.DbInit("run1").ok());
  RemoveAll(root + "/.nexus/containers/run1.meta");

  // ---- container metadata persistence routed through the runtime ----
  CHECK("container create", mgr.ContainerCreate("c1", 0, 0, "").ok());
  CHECK("duplicate container -> bad state",
        mgr.ContainerCreate("c1", 0, 0, "").code == DbError::kBadState);
  std::string status;
  CHECK("container status", mgr.ContainerStatus("c1", &status).ok());
  CHECK("status reports CREATED", Contains(status, "CREATED"));
  CHECK("meta file persisted",
        std::filesystem::exists(root + "/.nexus/containers/c1.meta"));
  {
    Manager rehydrated(root);
    std::string st;
    CHECK("rehydrate from disk", rehydrated.ContainerStatus("c1", &st).ok());
    CHECK("rehydrated state CREATED", Contains(st, "CREATED"));
  }
  CHECK("container delete", mgr.ContainerDelete("c1").ok());
  CHECK("meta file removed",
        !std::filesystem::exists(root + "/.nexus/containers/c1.meta"));
  CHECK("status after delete -> not found",
        mgr.ContainerStatus("c1", &status).code == DbError::kNotFound);
  CHECK("delete unknown container -> not found",
        mgr.ContainerDelete("ghost").code == DbError::kNotFound);

  // ---- Router parsing ----
  Command cmd;
  std::string err;
  CHECK("parse help", ParseArgs({"help"}, &cmd, &err) == 0 &&
                          cmd.kind == CommandKind::kHelp);
  CHECK("parse no args -> usage", ParseArgs({}, &cmd, &err) == 2);
  CHECK("parse unknown command -> usage",
        ParseArgs({"frobnicate"}, &cmd, &err) == 2);
  CHECK("parse insert", ParseArgs({"insert", "db1", "t1", "deadbeef"}, &cmd,
                                  &err) == 0 &&
                            cmd.data.size() == 4 && cmd.data[0] == 0xde &&
                            cmd.data[3] == 0xef);
  CHECK("parse insert bad hex -> invalid",
        ParseArgs({"insert", "db1", "t1", "zz"}, &cmd, &err) == 3);
  CHECK("parse insert odd hex -> invalid",
        ParseArgs({"insert", "db1", "t1", "abc"}, &cmd, &err) == 3);
  CHECK("parse insert wrong arity -> usage",
        ParseArgs({"insert", "db1", "t1"}, &cmd, &err) == 2);
  CHECK("parse get bad page -> invalid",
        ParseArgs({"get", "db1", "t1", "x", "0"}, &cmd, &err) == 3);
  CHECK("parse container create flags",
        ParseArgs({"container", "create", "c9", "--memory", "1024", "--cpu",
                   "50"},
                  &cmd, &err) == 0 &&
            cmd.memory == 1024 && cmd.cpu == 50);
  CHECK("parse container create bad cpu -> invalid",
        ParseArgs({"container", "create", "c9", "--cpu", "250"}, &cmd,
                  &err) == 3);
  CHECK("parse container start", ParseArgs({"container", "start", "c9"}, &cmd,
                                           &err) == 0 &&
                                   cmd.kind == CommandKind::kContainerStart);

  // ---- Router numeric boundary / overflow parsing ----
  // Page ids: kInvalidId (0xFFFFFFFF) is reserved and rejected; the largest
  // valid id is 0xFFFFFFFE. Values above that (up to 2^32 and far beyond) must
  // be rejected, never silently wrapped.
  CHECK("parse get max page id accepted",
        ParseArgs({"get", "db1", "t1", "4294967294", "0"}, &cmd, &err) == 0 &&
            cmd.page == 4294967294u);
  CHECK("parse get reserved page id rejected -> invalid",
        ParseArgs({"get", "db1", "t1", "4294967295", "0"}, &cmd, &err) == 3);
  CHECK("parse get page id 2^32 rejected -> invalid",
        ParseArgs({"get", "db1", "t1", "4294967296", "0"}, &cmd, &err) == 3);
  CHECK("parse get huge page id rejected -> invalid",
        ParseArgs({"get", "db1", "t1", "99999999999999999999", "0"}, &cmd,
                  &err) == 3);
  CHECK("parse get empty page id rejected -> invalid",
        ParseArgs({"get", "db1", "t1", "", "0"}, &cmd, &err) == 3);
  CHECK("parse get negative page id rejected -> invalid",
        ParseArgs({"get", "db1", "t1", "-1", "0"}, &cmd, &err) == 3);
  CHECK("parse get max slot accepted",
        ParseArgs({"get", "db1", "t1", "1", "65535"}, &cmd, &err) == 0 &&
            cmd.slot == 0xFFFF);
  CHECK("parse get slot 65536 rejected -> invalid",
        ParseArgs({"get", "db1", "t1", "1", "65536"}, &cmd, &err) == 3);
  CHECK("parse get huge slot rejected -> invalid",
        ParseArgs({"get", "db1", "t1", "1", "99999999999999999999"}, &cmd,
                  &err) == 3);

  // Memory: documented maximum is 2^48 - 1 bytes.
  CHECK("parse memory max accepted",
        ParseArgs({"container", "create", "c", "--memory", "281474976710655"},
                  &cmd, &err) == 0 &&
            cmd.memory == 281474976710655UL);
  CHECK("parse memory over max rejected -> invalid",
        ParseArgs({"container", "create", "c", "--memory", "281474976710656"},
                  &cmd, &err) == 3);
  CHECK("parse memory huge rejected -> invalid",
        ParseArgs({"container", "create", "c", "--memory",
                   "99999999999999999999"},
                  &cmd, &err) == 3);
  CHECK("parse cpu 0 accepted",
        ParseArgs({"container", "create", "c", "--cpu", "0"}, &cmd, &err) ==
                0 &&
            cmd.cpu == 0);
  CHECK("parse cpu 100 accepted",
        ParseArgs({"container", "create", "c", "--cpu", "100"}, &cmd, &err) ==
                0 &&
            cmd.cpu == 100);
  CHECK("parse cpu 101 rejected -> invalid",
        ParseArgs({"container", "create", "c", "--cpu", "101"}, &cmd, &err) ==
            3);
  CHECK("parse cpu huge rejected -> invalid",
        ParseArgs({"container", "create", "c", "--cpu",
                   "99999999999999999999"},
                  &cmd, &err) == 3);

  // ---- Router dispatch error propagation ----
  nexusdb::cli::Result r = Run(&mgr, {"scan", "db1", "missing_table"});
  CHECK("dispatch scan missing table -> not found",
        r.exit_code == nexusdb::cli::kNotFound);
  r = Run(&mgr, {"insert", "db1", "t1", "nothex"});
  CHECK("dispatch bad hex -> invalid", r.exit_code == nexusdb::cli::kInvalid);
  r = Run(&mgr, {"help"});
  CHECK("dispatch help -> ok", r.exit_code == nexusdb::cli::kOk &&
                                   Contains(r.output, "Usage"));
  r = Run(&mgr, {"table", "list", "db1"});
  CHECK("dispatch table list -> ok and lists t1",
        r.exit_code == nexusdb::cli::kOk && Contains(r.output, "t1"));

  RemoveAll(root);
  RemoveAll(cg);

  std::printf("\n=== %s (%d failure(s)) ===\n",
              g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
