# NexusDB

> An experimental OS-database integration system that explicitly connects container-level memory state with database buffer-pool management — built from scratch in C/C++ on Linux.

![Language](https://img.shields.io/badge/language-C%2B%2B17%20%2F%20C11-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20WSL2-orange.svg)
![Status](https://img.shields.io/badge/status-active-success.svg)
![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Build](https://img.shields.io/badge/build-CMake-red.svg)

---

## 📋 Table of Contents

- [Overview](#-overview)
- [Architecture](#-architecture)
- [Features](#-features)
- [Tech Stack](#-tech-stack)
- [Project Structure](#-project-structure)
- [Getting Started](#-getting-started)
- [Usage](#-usage)
- [Component Deep Dive](#-component-deep-dive)
- [Benchmarking](#-benchmarking)
- [Team](#-team)
- [License](#-license)

---

## 🔍 Overview

NexusDB is an **educational systems programming project** that bridges two traditionally separate concerns — container resource management and database engine internals. Most production databases treat OS-level memory constraints as external noise. NexusDB treats them as first-class signals.

The core idea:

> **Container memory pressure → OS-DB Bridge → Buffer Pool adaptation → LRU eviction → measurable performance change → Dashboard**

When the Linux kernel reports that a containerized database process is approaching its cgroup memory limit, NexusDB's OS-DB Bridge detects this, computes a new target buffer pool size, and commands the DB engine to shrink — triggering LRU eviction and freeing memory headroom, all without crashing or restarting the database.

**This is not a replacement for Docker or PostgreSQL.** It is a ground-up implementation that makes the OS-database feedback loop explicit and observable.

---

## 🏗️ Architecture

NexusDB is organized into **4 layers**:

```
┌─────────────────────────────────────────────────┐
│         Layer 1 — User Interface (CLI)          │
│         Commands / Query Input / Dashboard       │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│         Layer 2 — NexusDB Runtime               │
│   Runtime Coordinator · Container Engine        │
│   DB Engine Implementation · OS-DB Bridge       │
└──────┬─────────────────────────┬────────────────┘
       │                         │
┌──────▼──────┐         ┌────────▼───────────────┐
│  Layer 3    │         │  NexusDB Container      │
│  Container  │         │  DB Engine Instance     │
│  (Isolated) │         │  Buffer Pool · WAL      │
└──────┬──────┘         └────────────────────────┘
       │
┌──────▼──────────────────────────────────────────┐
│         Layer 4 — Linux Kernel                  │
│   namespaces · cgroups v2 · process scheduler   │
└─────────────────────────────────────────────────┘
```

### Data Flow

```
User → CLI → Runtime Coordinator → Container Engine → DB Engine Instance
                                                            │
                              cgroup metrics ← OS-DB Bridge ┘
                                    │
                              Adaptation Policy
                                    │
                         Buffer Pool target resize
                                    │
                              LRU Eviction → Memory freed
                                    │
                              Dashboard (live metrics)
```

---

## ✨ Features

- **Custom Container Runtime** — Creates isolated Linux containers using namespaces and cgroups v2 from scratch (no Docker dependency)
- **Full Database Engine** — Page-based storage, CRUD operations, SQL query processing with lexer and parser
- **Buffer Pool + LRU Eviction** — In-memory page cache with configurable size and LRU-based eviction
- **Write-Ahead Logging (WAL)** — Crash-safe writes with a full recovery manager
- **OS-DB Bridge** — Real-time cgroup memory monitoring with adaptive buffer pool resizing
- **Dynamic Resource Adaptation** — Buffer pool shrinks/grows in response to live container memory pressure
- **Live Dashboard** — ncurses-based terminal UI showing memory, CPU, cache hit ratio, and query latency
- **Benchmarking Suite** — Controlled workloads under 512MB / 256MB / 128MB memory limits with comparative results

---

## 🛠️ Tech Stack

| Layer | Technology | Why |
|-------|-----------|-----|
| Database Engine | C++17 | Performance, memory control, STL |
| Container Runtime | C11 | Direct syscall access, minimal overhead |
| OS-DB Bridge | C++17 | Interfaces with both C container layer and C++ DB layer |
| Build System | CMake 3.20+ | Mixed C/C++ project, cross-component linking |
| Linux Features | namespaces, cgroups v2 | Process isolation, resource enforcement |
| Dashboard | ncurses | Lightweight terminal UI, no GUI dependency |
| Testing | Google Test (C++) + Unity (C) | Separate test frameworks for each language |
| Platform | Ubuntu 22.04 / WSL2 | cgroups v2 support required |

---

## 📁 Project Structure

```
NexusDB/
│
├── include/                    ← All public headers (single source of truth)
│   ├── common/                 ← Shared types, error codes, config
│   ├── cli/                    ← CLI and command parser interfaces
│   ├── runtime/                ← Runtime coordinator interface
│   ├── db/                     ← All DB engine headers
│   ├── container/              ← Container runtime headers
│   └── bridge/                 ← OS-DB Bridge headers
│
├── src/                        ← Implementation only (no headers)
│   ├── main/                   ← Entry point
│   ├── cli/                    ← CLI + command parser
│   ├── runtime/                ← Runtime coordinator
│   ├── db/
│   │   ├── engine/             ← DB engine core
│   │   ├── manager/            ← Request routing
│   │   ├── query/              ← Lexer, parser, query processor
│   │   ├── storage/            ← Page + page storage
│   │   ├── buffer/             ← Buffer pool + frames
│   │   ├── cache/              ← LRU eviction
│   │   ├── transaction/        ← Transaction management
│   │   └── recovery/           ← WAL + crash recovery
│   ├── container/              ← C: namespaces, cgroups, lifecycle
│   └── bridge/                 ← OS-DB Bridge + adaptation policy
│
├── tests/
│   ├── db/                     ← Unit tests for all DB components
│   ├── container/              ← Unit tests for container runtime
│   └── integration/            ← End-to-end system tests
│
├── scripts/
│   ├── setup.sh                ← Environment setup
│   ├── setup_cgroups.sh        ← cgroups v2 configuration
│   ├── cleanup.sh              ← Teardown
│   └── run_benchmark.sh        ← Benchmark runner
│
├── benchmarks/
│   ├── workloads/              ← Benchmark workload definitions
│   └── results/                ← Recorded benchmark output
│
├── docs/                       ← Component documentation
├── data/                       ← Database files (runtime)
├── logs/                       ← Log output (runtime)
└── build/                      ← CMake build output
```

---

## 🚀 Getting Started

### Prerequisites

- **OS**: Ubuntu 22.04 or WSL2 (Windows 11) — cgroups v2 required
- **Compiler**: GCC 11+ or Clang 14+ (C++17 support)
- **CMake**: 3.20+
- **Libraries**: ncurses, pthread
- **Permissions**: sudo access (for namespace and cgroup setup)

```bash
# Verify cgroups v2
mount | grep cgroup2
# Should output: cgroup2 on /sys/fs/cgroup type cgroup2

# Install dependencies
sudo apt update
sudo apt install -y build-essential cmake libncurses-dev
```

### Build

```bash
# Clone the repository
git clone https://github.com/your-team/nexusdb.git
cd nexusdb

# Setup environment (cgroups, directories)
chmod +x scripts/*.sh
sudo ./scripts/setup.sh
sudo ./scripts/setup_cgroups.sh

# Build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Run Tests

```bash
# From build directory
make test

# Or run specific suite
./tests/db/engine_test
./tests/container/container_test
./tests/integration/db_container_test
```

---

## 💻 Usage

### Start NexusDB

```bash
# Launch the NexusDB runtime
sudo ./build/nexusdb
```

### CLI Commands

```bash
# Container management
nexus> create container --name mydb --memory 512MB --cpu 50%
nexus> start container mydb
nexus> stop container mydb
nexus> status container mydb

# Database queries (once container is running)
nexus> query mydb "INSERT INTO users VALUES (1, 'Alice')"
nexus> query mydb "SELECT * FROM users"
nexus> query mydb "UPDATE users SET name='Bob' WHERE id=1"
nexus> query mydb "DELETE FROM users WHERE id=1"

# Monitoring
nexus> dashboard          # Launch live ncurses dashboard
nexus> metrics mydb       # Print current resource metrics
```

### Dashboard

The live dashboard displays:

| Metric | Description |
|--------|-------------|
| Memory Limit | cgroup memory.max |
| Memory Usage | cgroup memory.current |
| CPU Usage | % of allocated CPU |
| Buffer Pool Size | Current target (pages) |
| Cache Hit Ratio | Hits / (Hits + Misses) |
| Query Latency | Average ms per query |

---

## 🔬 Component Deep Dive

### Database Engine (DB Engine)

The query path follows:

```
SQL String → Lexer → Parser → Query Processor → Buffer Pool → Page Storage
                                                      │
                                                  WAL (writes)
                                                      │
                                               Recovery Manager
```

- **Page size**: 4 KB (configurable via `include/common/config.h`)
- **Buffer pool**: fixed number of frames in RAM, backed by page storage on disk
- **LRU**: evicts least-recently-used page frame when pool is full or target is reduced
- **WAL**: every write is logged before being applied — enables crash recovery

### Container Runtime

Uses raw Linux syscalls (no libcontainer):

```c
// Namespace creation
clone(child_fn, stack, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET, args);

// cgroup v2 memory limit
write(fd, "536870912", len);  // 512MB to memory.max
```

### OS-DB Bridge — Adaptation Policy

```
Read memory.current and memory.max from cgroup fs
    │
    ▼
pressure_ratio = memory.current / memory.max

if pressure_ratio > 0.85:   → shrink buffer pool by 20%
if pressure_ratio < 0.60:   → grow buffer pool by 10%
else:                        → no action
```

---

## 📊 Benchmarking

Run controlled experiments across three memory constraint levels:

```bash
sudo ./scripts/run_benchmark.sh
```

| Memory Limit | Expected Behavior |
|-------------|-------------------|
| 512 MB | Baseline — buffer pool operates near full capacity |
| 256 MB | Moderate pressure — bridge triggers 1–2 shrink events |
| 128 MB | High pressure — frequent eviction, measurable latency increase |

Results are saved to `benchmarks/results/` with query latency, cache hit ratio, and buffer pool size over time.

---

## 👥 Team

| Member | Role | Ownership |
|--------|------|-----------|
| **Digvijay Singh Rajawat** | Team Lead | Database Engine · Manager/Router · CLI · WAL · Recovery · Final Integration |
| **Ayush Yadav** | Systems Engineer | Container Runtime · Linux Layer · namespaces · cgroups v2 · Process lifecycle |
| **Md Abu Rehan** | Bridge & Observability | OS-DB Bridge · Monitoring · Dynamic Adaptation · Dashboard · Benchmarking |

### End-to-End Ownership Flow

```
Container [Ayush] → DB Process [Digvijay] → RAM / Buffer Pool [Digvijay]
      ↑                                              ↓
cgroup metrics [Ayush] ← OS-DB Bridge [Rehan] ← memory pressure
      └──────────────────────────────────────────────┘
                    Dashboard [Rehan]
```

---

## 🎓 About This Project

NexusDB was built as a final systems programming project to explore the intersection of **operating systems** and **database internals**. The central research question:

> *Can container-level memory signals be used to drive real-time database buffer management — and can we observe the performance effects?*

**Key concepts implemented from scratch:**
- Linux process isolation (namespaces)
- Resource enforcement (cgroups v2)
- Page-based database storage
- Buffer pool management with LRU eviction
- Write-ahead logging and crash recovery
- Cross-layer adaptive resource management

---

## 📄 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

<div align="center">

**NexusDB** — Where the OS meets the Database

*Built with 🔧 by Digvijay, Ayush & Rehan*

</div>