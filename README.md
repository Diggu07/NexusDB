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
  runtime resizing** (`Resize`); and a minimal **`db_engine`** executable the
  container launches as its workload — it opens the storage stack, stays alive
  until `SIGTERM`/`SIGINT`, then flushes and closes cleanly (no SQL or network
  layer yet). All fallible calls report through a `Status` / `DbError` result
  layer (no exceptions across the API).
- **OS-DB Bridge, adaptation & dashboard (C11 / C++17)** — `resource_monitor`
  reads real cgroup v2 statistics (`memory.current` / `memory.max` / `cpu.stat`)
  with explicit per-field availability (nothing is fabricated);
  `adaptation_policy` turns memory pressure into a grow/shrink decision; a C++
  adapter routes resize requests through the engine's public `BufferPool::Resize`;
  and a dashboard renders live metrics as plain text or via an optional ncurses
  UI, printing every unavailable metric as `unavailable`.
- **Benchmarking** — a harness runs the **real** engine under a container memory
  limit, sampling resources and adaptation decisions, and writes self-describing
  result files (it refuses to emit numbers when the engine is absent rather than
  substituting synthetic data).
- **Build & tests** — CMake (primary) and a root `Makefile` (parallel path)
  build the container runtime, the DB engine, the bridge/dashboard/benchmark
  targets and their tests; 17 tests are registered with CTest.

### Planned

- Write-ahead logging (WAL) and crash recovery
- Query processing / CRUD operations and a runtime coordinator + CLI

> Everything under **Planned** is a design target and is **not** implemented on
> this branch yet.

---

## Architecture (target design)

NexusDB is organised as four layers. The container engine, database engine,
OS-DB Bridge and dashboard exist; the CLI and runtime coordinator are planned.

```
┌─────────────────────────────────────────────────┐
│         Layer 1 — User Interface (CLI)          │  planned
│         Commands / Query Input / Dashboard       │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│         Layer 2 — NexusDB Runtime               │  partly implemented
│   Runtime Coordinator · Container Engine        │
│   DB Engine Implementation · OS-DB Bridge       │
└──────┬─────────────────────────┬────────────────┘
       │                         │
┌──────▼──────┐         ┌────────▼───────────────┐
│  Layer 3    │         │  NexusDB Container      │
│  Container  │         │  DB Engine Instance     │  implemented
│  (Isolated) │         │  Buffer Pool · WAL      │  (no WAL yet)
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
| Database Engine | C++17 | Page, PageStorage, BufferPool (LRU + resize), `db_engine` workload |
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
│   ├── db/                     ← Page, PageStorage, BufferPool      [implemented]
│   ├── bridge/                 ← resource monitor, adaptation       [implemented]
│   ├── dashboard/              ← metrics + presentation API         [implemented]
│   └── benchmark/              ← benchmark harness contract         [implemented]
├── src/
│   ├── container/              ← C: registry + lifecycle + process  [implemented]
│   │                             + namespaces + cgroups v2          [implemented]
│   ├── db/{storage,buffer,engine} ← Page, PageStorage, BufferPool,  [implemented]
│   │                             db_engine workload executable       [implemented]
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

A `db_engine` executable is built (the container's workload — storage + buffer
pool, no SQL/CLI yet); there is still no daemon, query CLI or client/server
protocol.

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
