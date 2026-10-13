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

- **Container foundation (C)** — an in-memory container registry (64 slots) and
  lifecycle: `container_create` → `container_start` → `container_stop` →
  `container_delete`, plus `container_status` / `container_get`, name/limit
  validation, and a portable **placeholder** child process (Linux
  `fork/exec/waitpid`; a Windows `ping` shim so the demo runs on dev machines
  without WSL). No real OS isolation yet.
- **Page storage (C++17)** — a fixed 4 KiB `Page` value type and a `PageStorage`
  that maps dense pages onto a single database file: `AllocatePage`,
  `ReadPage`, write-through `WritePage`, `NumPages`, `PageExists`, `Flush`,
  `Close`. Opening never truncates an existing file. All fallible calls report
  through a `Status` / `DbError` result layer (no exceptions across the API).
- **Build & tests** — CMake (primary) and a root `Makefile` (parallel path)
  build both the container lifecycle test and the page-storage test; the tests
  are registered with CTest.

### Under development (Database Engine — Digvijay)

- Buffer pool + page frames
- LRU eviction and dynamic buffer-pool resizing
- CRUD operations and query processing

### Planned

- Write-ahead logging (WAL) and crash recovery
- Runtime coordinator and CLI
- Container namespaces and cgroups v2 isolation
- OS-DB Bridge, adaptation policy, live dashboard, benchmarking suite

> Everything under **Under development** and **Planned** is a design target.
> It is **not** implemented on the current branch yet.

---

## Architecture (target design)

NexusDB is planned as four layers. Only the container foundation and page
storage exist so far.

```
┌─────────────────────────────────────────────────┐
│         Layer 1 — User Interface (CLI)          │  planned
│         Commands / Query Input / Dashboard       │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│         Layer 2 — NexusDB Runtime               │  planned
│   Runtime Coordinator · Container Engine        │
│   DB Engine Implementation · OS-DB Bridge       │
└──────┬─────────────────────────┬────────────────┘
       │                         │
┌──────▼──────┐         ┌────────▼───────────────┐
│  Layer 3    │         │  NexusDB Container      │
│  Container  │         │  DB Engine Instance     │  partly implemented
│  (Isolated) │         │  Buffer Pool · WAL      │  (page storage only)
└──────┬──────┘         └────────────────────────┘
       │
┌──────▼──────────────────────────────────────────┐
│         Layer 4 — Linux Kernel                  │  planned
│   namespaces · cgroups v2 · process scheduler   │
└─────────────────────────────────────────────────┘
```

---

## Tech Stack

| Layer | Technology | Status |
|-------|-----------|--------|
| Database Engine | C++17 | page storage implemented |
| Container Runtime | C11 | foundation implemented |
| OS-DB Bridge | C++17 | planned |
| Build System | CMake 3.16+ / Make | implemented |
| Container isolation | namespaces, cgroups v2 | planned |
| Dashboard | ncurses | planned |
| Testing | hand-written C/C++ test programs via CTest | implemented |

---

## Project Structure

```
NexusDB/
├── include/
│   ├── common/                 ← shared config + DbError/Status     [implemented]
│   ├── container/              ← container runtime public API       [implemented]
│   └── db/                     ← Page, PageStorage                  [implemented]
├── src/
│   ├── container/              ← C: registry + lifecycle + process  [implemented]
│   └── db/storage/             ← Page + PageStorage                 [implemented]
├── tests/
│   ├── container/              ← container lifecycle test           [implemented]
│   └── db/                     ← page storage test                  [implemented]
├── docs/                       ← component documentation            [implemented]
├── CMakeLists.txt              ← primary build                      [implemented]
├── Makefile                    ← parallel build/test path            [implemented]
└── (planned) src/db/{engine,buffer,cache,query,recovery}, src/cli,
    src/runtime, src/bridge, src/main, scripts/, benchmarks/
```

---

## Getting Started

### Prerequisites

- **Compiler**: GCC 11+ or Clang 14+ (C++17 / C11)
- **CMake**: 3.16+
- **Make** (optional): a `Makefile` path is provided for convenience

The Linux/WSL2 target is required for the planned namespace/cgroup work; the
container foundation and page storage build and run on a Windows dev machine
with MinGW as well.

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

There is no daemon, CLI, or database executable yet — only the test binaries
built above.

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
