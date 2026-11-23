# NexusDB

> An experimental OS-database integration project exploring how container-level memory pressure can drive database buffer-pool management — built from scratch in C/C++ (Linux / WSL2 target).

![Language](https://img.shields.io/badge/language-C%2B%2B17%20%2F%20C11-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20WSL2-orange.svg)
![Status](https://img.shields.io/badge/status-under%20development-yellow.svg)
![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Build](https://img.shields.io/badge/build-CMake-red.svg)

---

## Overview

NexusDB is an educational systems programming project that aims to make the OS↔database feedback loop explicit and observable:

> Container memory pressure → OS-DB Bridge → Buffer Pool adaptation → LRU eviction → measurable performance change → Dashboard

The goal: when the Linux kernel reports that a containerized database process is approaching its cgroup memory limit, detect it, compute a new buffer-pool target, and shrink the pool (evicting pages) without crashing or restarting the database.

**This is not a replacement for Docker or PostgreSQL.** It is a ground-up implementation built in phases.

---

## Project Status

This project is developed in vertical slices. The table below separates what is
actually in the tree from what is still being designed or built.

### Implemented

- **Container runtime (C11)** — an in-memory registry (64 slots) and lifecycle:
  `container_create` → `container_start` → `container_stop` → `container_delete`,
  plus `container_status` / `container_get`, name/limit validation. On Linux the
  lifecycle provisions a real **cgroup v2** hierarchy (`memory.max` / `cpu.max`)
  and `container_start` launches its workload through a **gated child** that is
  attached to the cgroup *before* `exec` and placed in **Linux namespaces** (PID,
  mount, network, UTS, IPC). A container may configure a `db_engine` executable
  as its workload; without one it falls back to the built-in placeholder child.
  A portable shim keeps the demo runnable on Windows dev machines without WSL.
- **Database engine (C++17)** — a fixed 4 KiB `Page` value type and a
  `PageStorage` that maps dense pages onto a single database file
  (`AllocatePage`, `ReadPage`, write-through `WritePage`, `NumPages`,
  `PageExists`, `Flush`, `Close`); a `BufferPool` over it with **LRU eviction and
  runtime resizing** (`Resize`); a `SlottedPage` record layer, a multi-page
  `HeapTable`, a persistent name→table `Catalog`, and a `QueryExecutor` for
  table-scoped CRUD + scans; and a **write-ahead log** with CRC-32 integrity and
  crash recovery, tied together by a `Database` facade that runs recovery before
  any read/write and enforces WAL-before-data page ordering. A **`db_engine`**
  executable the container launches as its workload opens the full `Database`,
  stays alive until `SIGTERM`/`SIGINT`, then flushes and closes cleanly (no SQL or
  network layer). All fallible calls report through a `Status` / `DbError` result
  layer (no exceptions across the API).
- **Manager / Router / CLI (C++17)** — a `Manager` that coordinates the container
  runtime and the database engine, a `Router` that validates and dispatches
  arguments, and a `nexusdb` **CLI** providing an end-to-end workflow: create a
  named database container, run its lifecycle, and create tables / insert / get /
  update / delete / scan records as hexadecimal. Container metadata is persisted
  to disk (not only a process-local registry); the CLI is a one-shot process with
  stable exit codes. When a container is RUNNING, its engine exposes a private
  Unix-domain control socket and database commands are proxied to it; when the
  container is stopped, the CLI opens the database file directly.
- **OS-DB Bridge, adaptation & dashboard (C11 / C++17)** — `resource_monitor`
  reads real cgroup v2 statistics (`memory.current` / `memory.max` / `cpu.stat`)
  with explicit per-field availability (nothing is fabricated);
  `adaptation_policy` turns memory pressure into a grow/shrink decision; a C++
  adapter routes resize requests through the engine's public `BufferPool::Resize`;
  and a dashboard renders live metrics as plain text or via an optional ncurses
  UI, printing every unavailable metric as `unavailable`. The `engine_adaptation`
  test drives the production policy + adapter against a **real** `BufferPool` and
  `Database`: bridge-requested resize, pinned-frame shrink refusal, dirty-eviction
  write-ahead ordering, and record survival across resize and close/reopen (WAL
  recovery). CPU is measured/displayed but never drives resizing.
- **Benchmarking** — a harness runs the **real** engine under a container memory
  limit, sampling resources and adaptation decisions, and writes self-describing
  result files (it refuses to emit numbers when the engine is absent rather than
  substituting synthetic data). Its workload is buffer-pool page access under real
  memory pressure (not record/SQL); record-level resize + recovery correctness is
  covered by the `engine_adaptation` test.
- **Build & tests** — CMake (primary) and a root `Makefile` (parallel path)
  build the container runtime, the DB engine, the Manager/Router/CLI, the
  bridge/dashboard/benchmark targets and their tests; 29 tests are registered
  with CTest.

### Planned

- SQL parsing/execution and a type system
- Transactions (ACID / multi-statement atomicity)
- Networked client-server protocol (the local Unix-domain control socket is already implemented)
- Distributed query execution / replication

> Everything under **Planned** is a design target and is **not** implemented on
> this branch yet. In particular the CLI is **not** a SQL shell: it exposes only
> the table/record operations the engine actually supports (see *CLI* below).

---

## Architecture (target design)

NexusDB is organised as four layers. The container engine, database engine,
OS-DB Bridge, dashboard, benchmark harness and CLI all exist on this branch.

```
┌─────────────────────────────────────────────────┐
│         Layer 1 — User Interface (CLI)          │  implemented
│         nexusdb CLI (one-shot commands)          │  (no SQL shell)
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│         Layer 2 — NexusDB Runtime               │  implemented
│   Router · Manager · Container Engine           │
│   DB Engine Implementation · OS-DB Bridge       │
└──────┬─────────────────────────┬────────────────┘
       │                         │
┌──────▼──────┐         ┌────────▼───────────────┐
│  Layer 3    │         │  NexusDB Container      │
│  Container  │         │  DB Engine Instance     │  implemented
│  (Isolated) │         │  Buffer Pool · WAL      │  (WAL + recovery)
└──────┬──────┘         └────────────────────────┘
       │
┌──────▼──────────────────────────────────────────┐
│         Layer 4 — Linux Kernel                  │  implemented
│   namespaces · cgroups v2 · process scheduler   │  (Linux/WSL2)
└─────────────────────────────────────────────────┘
```

---

## Tech Stack

| Layer | Technology | Status |
|-------|-----------|--------|
| Database Engine | C++17 | Page, PageStorage, BufferPool, SlottedPage, HeapTable, Catalog, QueryExecutor, WAL + recovery, `Database`, `db_engine` workload |
| Manager / CLI | C++17 | Manager + Router + `nexusdb` CLI (container lifecycle + table CRUD) |
| Container Runtime | C11 | registry + lifecycle + gated namespaced child |
| OS-DB Bridge | C11 / C++17 | resource monitor + adaptation policy + resize adapter |
| Build System | CMake 3.16+ / Make | implemented |
| Container isolation | namespaces, cgroups v2 | implemented (Linux / WSL2) |
| Dashboard | ncurses (optional) + plain text | implemented |
| Testing | hand-written C/C++ test programs via CTest | implemented |

---

## Project Structure

```
NexusDB/
├── include/
│   ├── common/                 ← shared config + DbError/Status     [implemented]
│   ├── container/              ← container runtime public API       [implemented]
│   ├── db/                     ← Page, PageStorage, BufferPool,     [implemented]
│   │                             record/catalog/query, WAL, Database  [implemented]
│   ├── manager/                ← Manager + Router public API        [implemented]
│   ├── bridge/                 ← resource monitor, adaptation       [implemented]
│   ├── dashboard/              ← metrics + presentation API         [implemented]
│   └── benchmark/              ← benchmark harness contract         [implemented]
├── src/
│   ├── container/              ← C: registry + lifecycle + process  [implemented]
│   │                             + namespaces + cgroups v2          [implemented]
│   ├── db/{storage,buffer,record,catalog,query,wal,database,engine} [implemented]
│   │                             Page → ... → WAL/Database, db_engine  [implemented]
│   ├── manager/                ← Manager + Router (C++)             [implemented]
│   ├── cli/                    ← nexusdb CLI entry point            [implemented]
│   ├── bridge/                 ← resource monitor + policy + adapter [implemented]
│   ├── dashboard/              ← metrics + text/ncurses renderers    [implemented]
│   └── benchmark/              ← harness + engine workload provider  [implemented]
├── tests/                      ← container + db + bridge + dashboard [implemented]
│                                 + benchmark tests (CTest), Bash     [implemented]
│                                 prompt-integration test             [implemented]
├── examples/                   ← container / dashboard drivers       [implemented]
├── benchmarks/                 ← runner, workloads, results          [implemented]
├── docs/                       ← component documentation             [implemented]
├── scripts/                    ← cgroup setup / cleanup helpers       [implemented]
│                                 + Bash prompt integration           [implemented]
├── CMakeLists.txt              ← primary build                       [implemented]
└── Makefile                    ← parallel build/test path            [implemented]
```

---

## Getting Started

### Prerequisites

- **Compiler**: GCC 11+ or Clang 14+ (C++17 / C11)
- **CMake**: 3.16+
- **Make** (optional): a `Makefile` path is provided for convenience

The Linux/WSL2 target is required for real namespace/cgroup isolation; the
container runtime, database engine, bridge, dashboard and their tests build and
run on a Windows dev machine with MinGW as well (cgroup/namespace checks then
report `ENV-LIMITED` instead of passing).

### Build

```bash
cmake -S . -B build
cmake --build build
```

### Run Tests

```bash
ctest --test-dir build --output-on-failure
```

### Make alternative

```bash
make          # mingw32-make on Windows
make test
```

Two executables are built: `db_engine` (the container's workload — the full
`Database` with WAL + recovery) and `nexusdb` (the CLI). There is no separate
daemon: when a container is RUNNING, its `db_engine` exposes a private
Unix-domain control socket (`<data-dir>/nexusdb.sock`) that the CLI uses. If
the data directory's filesystem cannot host Unix sockets (WSL2 drives under
`/mnt/...`), the engine relocates the socket and records the real path in
`<data-dir>/nexusdb.sock.path`; `start` verifies the socket with a `ping`
before reporting RUNNING.

### CLI

The recommended user-facing interface is the **`nexus`** wrapper (see
[Shell integration](#shell-integration--container-aware-bash-prompt)): the
commands below work verbatim as `nexus <command>` — the wrapper forwards
argv, stdout/stderr, and exit codes to this CLI unchanged, and additionally
provides `nexus enter/leave/exit/context` for shell-context selection and a
unified `nexus help`. Invoke `build/nexusdb ...` directly when avoiding the
Bash wrapper (scripts, non-bash shells).

The `nexusdb` CLI is a one-shot process per command. The database for a name
lives in `$NEXUSDB_DATA_ROOT/<name>` (default `./nexusdb_data`); container
metadata is persisted alongside it under `<root>/.nexus/`.

```bash
build/nexusdb help
build/nexusdb create <name> [--memory <size>] [--cpu <percent>] [--engine <path> | --no-engine]
build/nexusdb start|stop|status|delete <name>
build/nexusdb list
build/nexusdb exec <name> [<command> [args...]]
build/nexusdb db init <name>
build/nexusdb table create <name> <table>
build/nexusdb table list <name>
build/nexusdb insert <name> <table> <hex-data>
build/nexusdb get <name> <table> <page-id> <slot-id>
build/nexusdb update <name> <table> <page-id> <slot-id> <hex-data>
build/nexusdb delete <name> <table> <page-id> <slot-id>
build/nexusdb scan <name> <table>
```

- The long forms `nexusdb container ...` and `nexusdb db ...` remain stable for
  scripts; the short forms above are aliases.
- `--memory` accepts plain bytes or binary suffixes (`B`, `K`/`KB`, `M`/`MB`,
  `G`/`GB`), e.g. `256M` or `1G`; `0` means unlimited. `--cpu` is a percentage
  (0..100).
- `create` launches `db_engine` by default (resolved from `--engine`, then
  `$NEXUSDB_DB_ENGINE`, then a sibling `db_engine`). `--no-engine` keeps the
  placeholder workload.
- On Linux/WSL2, `start` automatically re-execs under a transient
  `systemd-run --user --scope` when the current shell is outside the delegated
  user cgroup, so `nexusdb start` needs no prefix. The workload ignores
  `SIGHUP`, so it keeps running after the launching terminal closes.
- Data commands on a **RUNNING** container are proxied to the engine over its
  control socket, so they are safe while it holds the files open. On a stopped
  container they open the database directly (running WAL recovery on open).
  Persisted data survives `stop`, CLI exit, and process restart.
- `exec <name>` with no command is an interactive control-request loop;
  `exec <name> ping|stats` issues a single request.
- Record payloads are hexadecimal (even length), e.g. `insert db1 t1 deadbeef`.
- Exit codes: `0` ok, `1` operational error, `2` usage, `3` invalid argument,
  `4` not found.
- Unsupported (and not claimed): SQL, transactions, networking, distributed
  queries. The CLI is not a SQL shell.

### Shell integration — container-aware Bash prompt

Bash integration lives in `scripts/bash-integration.sh` and keeps its state
entirely in the interactive shell (no background process, no changes to the
CLI binaries or scripts folder besides this file). A standalone C/C++
executable cannot modify its parent shell, so the wrapper is a Bash function:
`nexus` is the recommended single entry point for ALL NexusDB operations.
Container/database commands remain the compiled `nexusdb` binary's business
(the wrapper only forwards); the shell integration stays responsible for the
prompt and the parent shell's context.

```text
no active context:   digvijay@MSI:/mnt/d/NexusDB$
nexus start testdb   digvijay@MSI:/mnt/d/NexusDB$:testdb>>  (start also selects it)
nexus enter testdb   digvijay@MSI:/mnt/d/NexusDB$:testdb>>
nexus enter ayush    digvijay@MSI:/mnt/d/NexusDB$:ayush>>   (replaces, never stacks)
nexus shell testdb   testdb:/container>>                  (isolated shell; `/container` only)
nexus stop testdb    digvijay@MSI:/mnt/d/NexusDB$           (clears an active context)
nexus leave / exit   digvijay@MSI:/mnt/d/NexusDB$
nexus context        Active container: testdb  (or: No container context is selected.)
```

Auto-isolation is the default: `nexus start <name>` and `nexus enter <name>`
drop straight into the isolated shell (same as `nexus shell <name>`). No config
is needed. To disable it globally, set `export NEXUSDB_ENTER_AUTO_SHELL=0` in
`~/.bashrc`; `nexus start <name> --no-shell` and `nexus enter --context <name>`
opt out per command.

If the current shell is outside `/sys/fs/cgroup/nexusdb/...`, `nexus start`
tries to move it into `/nexusdb/devshell` with cached `sudo`. If that is not
available, run it manually once per terminal:

```bash
mkdir -p /sys/fs/cgroup/nexusdb/devshell
echo $$ | sudo tee /sys/fs/cgroup/nexusdb/devshell/cgroup.procs >/dev/null
```

Inside the isolated shell, use the `nexusdb` CLI (the
`nexus` Bash function lives on the host); it is on `PATH` at `/tmp/nexusdb`
with `NEXUSDB_DATA_ROOT=/.nexusdb-data` (the same data dir is also mounted at
`/container`), and the live engine socket from
`nexusdb.sock.path` is mounted at its real path. Shortcuts avoid repeating the
CLI and container name: `status`, `list`, `exec`/`run`, `init`, `db`, `table`,
`insert`, `get`, `update`, `delete`, `scan` (run `nxhelp` inside for the list).
A live right-side panel shows the container's memory limit/used/free, CPU
limit/used, process count and state on every prompt; set `NEXUSDB_DASHBOARD=0`
to hide it.

End-to-end lifecycle with auto-isolation enabled:

```bash
nexus help                             # unified reference (CLI help + shell context)
nexus create testdb --memory 256M --cpu 50
nexus start testdb                     # starts, then drops into testdb:/container>>

# now inside testdb:/container>> — all DB commands run with filesystem isolation
status
list
init
table create students
insert students 48656c6c6f
scan students
get students <page> <slot>
exec ping
exit                                   # back to the host shell

nexus stop testdb
nexus delete testdb
```

Without auto-isolation, use `nexus shell testdb` explicitly before the same
shortcuts.

What it does and does not do:

- `nexus enter <name>` by default launches the isolated shell for an existing
  container. The name must exist (validated with the real CLI, exit code 4 →
  `no such container`, so a failed enter leaves prompt and context untouched)
  and must match the runtime's name rules (`[A-Za-z0-9_-]`, shorter than 64
  chars). The container does **not** need to be running. Use
  `nexus enter --context <name>` for the lightweight, non-isolated prompt-only
  behavior.
- `nexus start <name>` starts through the real CLI and, on success, drops
  straight into the isolated shell. `nexus start <name> --no-shell` keeps it
  context-only. `nexus stop/delete <name>` clears the context if active.
- `nexus shell <name>` launches a real isolated shell via `bwrap`: the container
  data directory is the only writable path, mounted at `/container` (also `HOME`),
  with private mount/PID/net/IPC/UTS namespaces. `ls` there shows container data,
  not the host repo. The `nexusdb` CLI is on `PATH` at `/tmp/nexusdb` with
  `NEXUSDB_DATA_ROOT=/.nexusdb-data` (the same data dir is also at `/container`),
  and the engine socket from `nexusdb.sock.path` is mounted at its real path, so
  DB commands work inside with isolation. It needs Linux user namespaces and
  `bwrap`; pass `-- <command> [args...]` for a one-shot.
- Auto-isolation is on by default; set `NEXUSDB_ENTER_AUTO_SHELL=0` to disable
  it globally. `nexus enter --context` and `nexus start --no-shell` opt out per
  command.
- All other `nexus ...` invocations are forwarded unchanged to the `nexusdb`
  CLI (argv, stdout/stderr, and exit codes preserved), so existing workflows
  are untouched. `nexus help` (also `-h`/`--help`) prints the CLI's
  authoritative help text and appends the shell-context section above — the
  wrapper never maintains a competing list of CLI commands; `nexus help
  <anything>` forwards for exact CLI parity.

How the prompt works: on source time the current `$PS1` is saved as the base.
A `PROMPT_COMMAND` hook prepended ahead of existing hooks rebuilds `$PS1` on
every render — the untouched base plus `:<context>>` only while
`$NEXUSDB_CONTEXT` is set. Therefore suffixes can never stack, switching
replaces them, leaving restores the original prompt byte-for-byte, and
dynamic elements (cwd, colors, `\w`) keep updating while a context is active.
No `eval` is involved; the container name must pass the strict name regex
before it can enter a prompt, and the CLI stays the authority on existence.

1. Load for the current terminal (WSL2/Ubuntu):
   ```bash
   source scripts/bash-integration.sh   # from anywhere: /abs/path also works
   ```
2. Enable for future Bash sessions (add to `~/.bashrc` yourself — never
   modified automatically):
   ```bash
   echo 'source ~/NexusDB/scripts/bash-integration.sh' >> ~/.bashrc
   ```
   (Adjust to the actual repo checkout path; source it after the distro's
   PS1 assignment lines so the saved base prompt is final.)
3. Disable: for the session, `nexus leave` (restore the prompt) or
   `nexus_unintegrate` (fully remove hook/state/functions). For future
   sessions, remove the `source` line from `~/.bashrc`.
4. Verify: `nexus context` prints the active container, `nexus enter bad
   names` are rejected, and `declare -f nexus` shows the integration is
   loaded. Sourcing is idempotent (double-`source` installs exactly one hook).

Resolution order for the CLI binary: `$NEXUSDB_CLI`, then `nexusdb` on
`$PATH`, then a sibling `build-linux-clean/build-linux/build/nexusdb` build
tree next to the integration script. Container validation honors
`$NEXUSDB_DATA_ROOT` when exported; otherwise it validates from the project
root the CLI lives in (the CLI's own `./nexusdb_data` default). Nothing is
hardcoded to a repository path. If the CLI cannot be found, `nexus` fails
fast with instructions (export `NEXUSDB_CLI`, extend `$PATH`, or build with
`cmake -S . -B build-linux-clean && cmake --build build-linux-clean`); it
never silently falls back to an unrelated binary.

Known limitations:

- The prompt suffix is Bash-session state; new terminals start with no
  context. Child scripts/shells inherit `$NEXUSDB_CONTEXT` but not the hook.
- Other PROMPT_COMMAND hooks that rewrite `$PS1` wholesale (e.g. certain
  themes) can fight the integration — hook order decides; chain carefully.
- `nexus` shadows only the new wrapper; it does not intercept `nexusdb`
  itself, so scripts calling the binary directly behave exactly as before.
- `$(nexus start <name>)` blocks until the container exits: the workload
  inherits the command substitution's stdout pipe, so the substitution never
  sees EOF. This is inherent to the CLI binary (identical with
  `$(nexusdb start ...)`) — redirect output to files instead. The Bash
  integration test captures streams through file redirections for this reason.

On Linux, tests that need a writable cgroup v2 hierarchy (container lifecycle,
enforcement) must run inside a **delegated** cgroup scope, otherwise they report
`ENV-LIMITED`. Unlike `nexusdb start`, the tests do not auto-delegate. For
example:

```bash
systemd-run --user --scope -q \
  env NEXUS_CGROUP_ROOT=/sys/fs/cgroup/user.slice/.../nexusdb \
  ctest --test-dir build --output-on-failure
```

---

## Team

| Member | Role | Ownership |
|--------|------|-----------|
| **Digvijay Singh Rajawat** | Team Lead | Database Engine · Manager/Router · CLI · WAL · Recovery · Final Integration |
| **Ayush Yadav** | Systems Engineer | Container Runtime · Linux Layer · namespaces · cgroups v2 · Process lifecycle |
| **Md Abu Rehan** | Bridge & Observability | OS-DB Bridge · Monitoring · Dynamic Adaptation · Dashboard · Benchmarking |

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
