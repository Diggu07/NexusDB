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
  stable exit codes. There is **no client/server protocol**: database commands
  open the database file directly and refuse to run while a container is RUNNING.
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
  bridge/dashboard/benchmark targets and their tests; 26 tests are registered
  with CTest.

### Planned

- SQL parsing/execution and a type system
- Transactions (ACID / multi-statement atomicity)
- Networking / a client-server request protocol
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
│                                 + benchmark tests (CTest)           [implemented]
├── examples/                   ← container / dashboard drivers       [implemented]
├── benchmarks/                 ← runner, workloads, results          [implemented]
├── docs/                       ← component documentation             [implemented]
├── scripts/                    ← cgroup setup / cleanup helpers       [implemented]
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
`Database` with WAL + recovery) and `nexusdb` (the CLI). There is still **no
daemon and no client/server protocol**.

### CLI

The `nexusdb` CLI is a one-shot process per command. The database for a name
lives in `$NEXUSDB_DATA_ROOT/<name>` (default `./nexusdb_data`); container
metadata is persisted alongside it under `<root>/.nexus/`.

```bash
build/nexusdb help
build/nexusdb container create <name> [--memory <bytes>] [--cpu <percent>] [--engine <path>]
build/nexusdb container start|stop|status|delete <name>
build/nexusdb db init <name>
build/nexusdb table create <name> <table>
build/nexusdb table list <name>
build/nexusdb insert <name> <table> <hex-data>
build/nexusdb get <name> <table> <page-id> <slot-id>
build/nexusdb update <name> <table> <page-id> <slot-id> <hex-data>
build/nexusdb delete <name> <table> <page-id> <slot-id>
build/nexusdb scan <name> <table>
```

- Record payloads are hexadecimal (even length), e.g. `insert db1 t1 deadbeef`.
- Data commands open the database file directly (running WAL recovery on open)
  and **refuse while the container is RUNNING** — stop it first. Persisted data
  survives `container stop`, CLI exit, and process restart.
- Exit codes: `0` ok, `1` operational error, `2` usage, `3` invalid argument,
  `4` not found.
- Unsupported (and not claimed): SQL, transactions, networking, distributed
  queries. The CLI is not a SQL shell.

On Linux, tests that need a writable cgroup v2 hierarchy (container lifecycle,
enforcement) must run inside a **delegated** cgroup scope, otherwise they report
`ENV-LIMITED`. For example:

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
