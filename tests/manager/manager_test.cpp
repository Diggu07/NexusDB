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
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "common/error.h"
#include "db/page.h"
#include "manager/manager.h"
#include "manager/router.h"

#if defined(__unix__) || defined(__APPLE__)
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
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

#if defined(__unix__) || defined(__APPLE__)
// Serves exactly one newline request on `socket_path`, replies "OK pong\n",
// unlinks the socket file, and exits 0. Runs in a forked helper process.
void ServePings(const std::string& socket_path) {
  sockaddr_un addr;
  char buf[256];
  const int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (lfd < 0)
    _exit(1);
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(addr.sun_path))
    _exit(1);
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
  (void)::unlink(socket_path.c_str());
  if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    _exit(1);
  if (::listen(lfd, 2) != 0)
    _exit(1);
  const int cfd = ::accept(lfd, nullptr, nullptr);
  if (cfd >= 0) {
    const ssize_t n = ::read(cfd, buf, sizeof(buf));
    if (n > 0)
      (void)::write(cfd, "OK pong\n", 8);
    ::close(cfd);
  }
  ::close(lfd);
  (void)::unlink(socket_path.c_str());
  _exit(0);
}

// Reaps the forked server helper without ever hanging or leaking it.
// `unblock` is set when the exchange failed, i.e. the helper is presumed
// still blocked in accept(): only then is it signaled, and only if it has
// not already exited (waitpid(WNOHANG) reaps an already-finished child, so
// a signaled pid is always a live one). A blocking waitpid retries on
// EINTR so an interrupted reap cannot silently drop the child.
void ReapServer(pid_t pid, bool unblock) {
  int status = 0;
  const pid_t first = ::waitpid(pid, &status, WNOHANG);
  if (first == pid)
    return;
  if (first == 0 && unblock)
    (void)::kill(pid, SIGTERM);
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}
#endif

std::string MakeBytes(std::initializer_list<std::uint8_t> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.begin()), bytes.size());
}

void WriteCgroupFiles(const std::string& cgroup_root,
                      const std::string& name) {
  const std::string dir = cgroup_root + "/" + name;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  {
    std::ofstream out(dir + "/memory.max", std::ios::trunc);
    out << "1048576\n";
  }
  {
    std::ofstream out(dir + "/memory.current", std::ios::trunc);
    out << "4096\n";
  }
  {
    std::ofstream out(dir + "/cpu.max", std::ios::trunc);
    out << "50000 100000\n";
  }
  {
    std::ofstream out(dir + "/cpu.stat", std::ios::trunc);
    out << "usage_usec 12345\n";
  }
  {
    std::ofstream out(dir + "/cgroup.procs", std::ios::trunc);
  }
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

  // ---- RUNNING guard at the Manager level (no cgroup needed: synthesized
  // metadata). The Router proxies to the engine instead of calling these
  // directly, but the guard remains the safety net for accidental misuse. ----
  WriteRunningMeta(root, "run1", 1 /* RUNNING */);
  CHECK("database op refused while container RUNNING",
        mgr.DbInit("run1").code == DbError::kBadState);
  WriteRunningMeta(root, "run1", 2 /* STOPPED */);
  CHECK("database op allowed when STOPPED", mgr.DbInit("run1").ok());
  RemoveAll(root + "/.nexus/containers/run1.meta");

#if defined(__unix__) || defined(__APPLE__)
  // ---- EngineRequest: newline protocol over a real Unix control socket ----
  // Variant 0 covers the default <data-dir>/nexusdb.sock location; variant 1
  // covers discovery via the engine's relocation marker
  // (<data-dir>/nexusdb.sock.path), which the engine writes when the data
  // directory's filesystem cannot host Unix sockets.
  {
    // EngineRequest("socktest-db", ...) resolves its control socket through
    // DbDir("socktest-db") == <data-root>/socktest-db, so every path below
    // must live in that exact directory -- a sibling directory would make
    // connect() fail with ENOENT and leave the helper blocked in accept().
    const std::string sock_root = mgr.DbDir("socktest-db");
    std::error_code sec;
    std::filesystem::create_directories(sock_root, sec);
    CHECK("socket test directory created",
          std::filesystem::is_directory(sock_root, sec));
    const std::string default_sock = sock_root + "/nexusdb.sock";
    const std::string relocated_sock = sock_root + "/relocated.sock";
    const std::string marker = sock_root + "/nexusdb.sock.path";
    (void)std::filesystem::remove(default_sock, sec);
    (void)std::filesystem::remove(relocated_sock, sec);
    (void)std::filesystem::remove(marker, sec);

    for (int variant = 0; variant < 2; ++variant) {
      if (variant == 1) {
        // Variant 0's helper unlinked the default socket on exit; publish a
        // relocation marker (with CRLF, exercising the trim) so the client
        // has to discover relocated.sock through it.
        std::ofstream mout(marker, std::ios::trunc);
        mout << relocated_sock << "\r\n";
      }
      const std::string server_path =
          (variant == 0) ? default_sock : relocated_sock;
      (void)std::fflush(nullptr);
      const pid_t server = ::fork();
      if (server < 0) {
        CHECK(variant == 0 ? "EngineRequest ping over primary control socket"
                           : "EngineRequest ping discovers relocated socket "
                             "via marker",
              false);
        continue;
      }
      if (server == 0) {
        ServePings(server_path); /* exits; never returns */
        _exit(1);
      }
      std::string pong;
      const Status es = mgr.EngineRequest("socktest-db", "ping", &pong);
      const bool served = es.ok() && Contains(pong, "OK pong");
      CHECK(variant == 0 ? "EngineRequest ping over primary control socket"
                         : "EngineRequest ping discovers relocated socket "
                           "via marker",
            served);
      if (!served) {
        if (es.ok())
          std::printf("  detail: unexpected response: %s\n", pong.c_str());
        else
          std::printf("  detail: %s\n", es.message.c_str());
      }
      // On failure the helper never got a connection and is still blocked in
      // accept(): ReapServer unblocks it so the reap cannot hang, and the
      // leftover socket file (which a signaled helper never unlinks) must
      // not leak into the next variant.
      ReapServer(server, !served);
      (void)std::filesystem::remove(server_path, sec);
    }
  }
#endif

  // ---- container metadata persistence routed through the runtime ----
  CHECK("container create", mgr.ContainerCreate("c1", 0, 0, "").ok());
  CHECK("duplicate container -> bad state",
        mgr.ContainerCreate("c1", 0, 0, "").code == DbError::kBadState);
  std::string status;
  CHECK("container status", mgr.ContainerStatus("c1", &status).ok());
  CHECK("status reports CREATED", Contains(status, "CREATED"));
  CHECK("meta file persisted",
        std::filesystem::exists(root + "/.nexus/containers/c1.meta"));
  WriteCgroupFiles(cg, "c1");
  std::string resources;
  CHECK("container resources", mgr.ContainerResources("c1", &resources).ok());
  CHECK("resources reports memory use",
        Contains(resources, "Memory used: 4096 bytes"));
  CHECK("resources reports memory limit",
        Contains(resources, "Memory limit: 1048576 bytes"));
  CHECK("resources reports cpu time",
        Contains(resources, "CPU time: 12345 usec"));
  CHECK("resources reports cpu limit", Contains(resources, "CPU limit: 50.0%"));
  CHECK("resources reports detached cgroup",
        Contains(resources, "Attached: no"));
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

  CHECK("container create with namespace isolation",
        mgr.ContainerCreate("ciso", 0, 0, "", true).ok());
  {
    std::ifstream in(root + "/.nexus/containers/ciso.meta");
    std::string line;
    std::string text;
    while (std::getline(in, line))
      text += line + "\n";
    CHECK("metadata records namespace request",
          Contains(text, "ns_pid=1") && Contains(text, "ns_mount=1") &&
              Contains(text, "ns_net=1") && Contains(text, "ns_uts=1") &&
              Contains(text, "ns_ipc=1"));
    Manager rehydrated(root);
    std::string st;
    CHECK("rehydrate isolated container",
          rehydrated.ContainerStatus("ciso", &st).ok());
  }
  CHECK("delete isolated container", mgr.ContainerDelete("ciso").ok());

#ifndef _WIN32
  // ---- process identity verification ----
  CHECK("container create for identity check",
        mgr.ContainerCreate("cid", 0, 0, "", false).ok());
  CHECK("container start for identity check", mgr.ContainerStart("cid").ok());
  {
    const std::string path = root + "/.nexus/containers/cid.meta";
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();
    const std::size_t pos = text.find("pid_start_ticks=");
    CHECK("identity metadata persisted", pos != std::string::npos);
    std::string corrupted = text;
    if (pos != std::string::npos) {
      const std::size_t end = corrupted.find('\n', pos);
      corrupted.replace(pos, end - pos, "pid_start_ticks=1");
    }
    {
      std::ofstream out(path, std::ios::trunc);
      out << corrupted;
    }
    Manager stale(root);
    CHECK("stale process identity refuses stop",
          stale.ContainerStop("cid").code == DbError::kBadState);
    {
      std::ofstream out(path, std::ios::trunc);
      out << text;
    }
    Manager restored(root);
    CHECK("restored process identity permits stop",
          restored.ContainerStop("cid").ok());
    CHECK("identity container delete",
          restored.ContainerDelete("cid").ok());
  }
#endif

  // ---- Router exit-code mapping ----
  CHECK("exit code ok", nexusdb::cli::ExitCodeFor(DbError::kOk) ==
                            nexusdb::cli::kOk);
  CHECK("exit code invalid", nexusdb::cli::ExitCodeFor(DbError::kInvalidArgument) ==
                                 nexusdb::cli::kInvalid);
  CHECK("exit code not found", nexusdb::cli::ExitCodeFor(DbError::kNotFound) ==
                                   nexusdb::cli::kNotFound);
  CHECK("exit code io error", nexusdb::cli::ExitCodeFor(DbError::kIoError) ==
                                  nexusdb::cli::kIoError);
  CHECK("exit code bad state", nexusdb::cli::ExitCodeFor(DbError::kBadState) ==
                                   nexusdb::cli::kBadState);
  CHECK("exit code buffer full",
        nexusdb::cli::ExitCodeFor(DbError::kBufferFull) ==
            nexusdb::cli::kBufferFull);
  CHECK("exit code out of memory",
        nexusdb::cli::ExitCodeFor(DbError::kOutOfMemory) ==
            nexusdb::cli::kOutOfMemory);

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

  // ---- Router shortcuts, memory units, help topics, delete disambiguation ----
  CHECK("parse create alias + units",
        ParseArgs({"create", "c9", "--memory", "256M", "--cpu-percent", "25"},
                  &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kContainerCreate &&
            cmd.memory == 268435456UL && cmd.cpu == 25);
  CHECK("parse create 1G",
        ParseArgs({"create", "c9", "--memory", "1G"}, &cmd, &err) == 0 &&
            cmd.memory == 1073741824UL);
  CHECK("parse create 1gb lowercase",
        ParseArgs({"create", "c9", "--memory", "1gb"}, &cmd, &err) == 0 &&
            cmd.memory == 1073741824UL);
  CHECK("parse create plain bytes",
        ParseArgs({"create", "c9", "--memory", "4096"}, &cmd, &err) == 0 &&
            cmd.memory == 4096UL);
  CHECK("parse create --no-engine",
        ParseArgs({"create", "c9", "--no-engine"}, &cmd, &err) == 0 &&
            cmd.engine.empty() && cmd.engine_explicit);
  CHECK("parse create --isolate",
        ParseArgs({"create", "c9", "--isolate"}, &cmd, &err) == 0 &&
            cmd.isolate_namespaces);
  CHECK("parse create bad unit -> invalid",
        ParseArgs({"create", "c9", "--memory", "16Q"}, &cmd, &err) == 3);
  CHECK("parse create fractional unit -> invalid",
        ParseArgs({"create", "c9", "--memory", "1.5G"}, &cmd, &err) == 3);
  CHECK("parse create unknown flag -> usage",
        ParseArgs({"create", "c9", "--bogus", "1"}, &cmd, &err) == 2);
  CHECK("parse list",
        ParseArgs({"list"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kContainerList);
  CHECK("parse resources short form",
        ParseArgs({"resources", "c1"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kContainerResources && cmd.name == "c1");
  CHECK("parse resources long form",
        ParseArgs({"container", "resources", "c1"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kContainerResources && cmd.name == "c1");
  CHECK("parse resources wrong arity -> usage",
        ParseArgs({"resources"}, &cmd, &err) == 2);
  CHECK("parse exec",
        ParseArgs({"exec", "c9", "ping"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kContainerExec && cmd.request == "ping");
  CHECK("parse delete <name> = container",
        ParseArgs({"delete", "c9"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kContainerDelete && cmd.name == "c9");
  CHECK("parse delete <name> <t> <p> <s> = record",
        ParseArgs({"delete", "db1", "t1", "1", "0"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kDelete && cmd.page == 1 && cmd.slot == 0);
  CHECK("parse create --help topic",
        ParseArgs({"create", "--help"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kHelp && cmd.help_topic == "create");
  CHECK("parse status --help topic",
        ParseArgs({"status", "--help"}, &cmd, &err) == 0 &&
            cmd.kind == CommandKind::kHelp && cmd.help_topic == "status");

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
  CHECK("resources dispatch fixture create",
        mgr.ContainerCreate("res1", 0, 0, "").ok());
  WriteCgroupFiles(cg, "res1");
  r = Run(&mgr, {"resources", "res1"});
  CHECK("dispatch resources -> ok and live memory",
        r.exit_code == nexusdb::cli::kOk &&
            Contains(r.output, "Memory used: 4096 bytes"));
  r = Run(&mgr, {"resources", "ghost"});
  CHECK("dispatch resources unknown -> not found",
        r.exit_code == nexusdb::cli::kNotFound);

  RemoveAll(root);
  RemoveAll(cg);

  std::printf("\n=== %s (%d failure(s)) ===\n",
              g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
