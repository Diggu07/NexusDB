# NexusDB Benchmarks (Rehan — Bridge & Observability)

Reproducible memory-limit benchmarks for the OS–DB Bridge end-to-end path:

```
container memory limit (cgroups v2)
  -> cgroup resource snapshot (bridge)
    -> memory-pressure adaptation policy
      -> buffer-pool resize request (DB engine public API)
```

## What is measured (real, observable quantities)

| Metric | Source | Notes |
|---|---|---|
| Buffer-pool resizing | `BufferPool::capacity()` before/after each policy request | Real `BufferPool::Resize` calls via the production adapter |
| GROW / SHRINK / HOLD decisions | Adaptation policy output | Counted per cgroup sample |
| Memory pressure | Bridge snapshot (`memory.current` / `memory.max`) | Never fabricated |
| Memory used / limit | Bridge snapshot | `memory.max == "max"` reported as unlimited |
| CPU usage | `nexus_bridge_cpu_percent` over two `cpu.stat` samples | Requires two CPU-capable samples |
| Cache hit/miss ratio | Harness probes `BufferPool::Contains()` **before** each `Fetch` | Harness-observed residency, not an engine counter |
| Operation latency | Wall-clock around each workload step (`Fetch` path) | Per-step `min`/`max`/`avg`/`total` milliseconds |

## What is NOT measured (and is never fabricated)

| Metric | Why unavailable |
|---|---|
| Engine-reported cache counters | `BufferPool` exposes no hit/miss counters (the harness observes residency via `Contains()` instead) |
| Query latency | There is no query processor on this branch |
| Real pool memory under kernel enforcement | The benchmark links the engine in-process (`engine_workload.cpp`) and joins its own process to the container cgroup, so accounting covers the running workload. The separate `db_engine` executable is now wired into the container lifecycle, but the benchmark measures the in-process pool |

Every result file prints these explicitly under `unavailable_metrics`.

## Scope and units

- **Units:** `MiB` in every scenario name, flag and result means **mebibytes
  (1024 × 1024 bytes)**, matching the cgroup `memory.max` the runner sets. It is
  never decimal megabytes.
- **Workload scope:** the engine workload (`engine_workload.cpp`) opens a
  `BufferPool` over a `PageStorage` and walks pages through the public API. It
  stress-tests **buffer-pool resizing under real memory pressure**; it is *not*
  a record/SQL workload and does not open the full `Database`.
- **Record-level correctness across resize + WAL recovery** is covered by the
  R4 integration test `tests/bridge/engine_adaptation_test.cpp`
  (`ctest -R engine_adaptation`), which inserts records through the real
  `QueryExecutor`, resizes the live pool via the bridge policy, and verifies the
  bytes survive `Close()` and a recovery reopen.
- `docs/container-runtime.md` documents the Bridge↔database interface and the
  adaptation policy; the thresholds live there too.

## Prerequisites

- Linux (Ubuntu 22.04 or WSL2) with **cgroups v2**.
- Toolchain: GCC/g++ 11+, `make`, `cmake`. The DB engine uses `<filesystem>`.
- cgroup delegation configured once per boot:

```bash
./scripts/setup.sh            # dirs + toolchain check
sudo ./scripts/setup_cgroups.sh   # /sys/fs/cgroup/nexusdb delegation
```

## Build

```bash
make benchmark        # -> build/nexus_benchmark   (C harness + C++ DB engine)
```

On the Windows dev machine (no engine) only a portable stub builds:

```cmd
mingw32-make all      :: -> build/nexus_benchmark_stub (refuses to run)
```

## Run

```bash
./benchmarks/run_benchmarks.sh
```

The runner executes the three required scenarios (sequential order):

| Scenario | Limit | Iterations | Pool start |
|---|---|---|---|
| `512MiB-sequential` | 512 MiB | 4096 | 256 frames |
| `256MiB-sequential` | 256 MiB | 4096 | 256 frames |
| `128MiB-sequential` | 128 MiB | 4096 | 256 frames |

Run a single scenario manually:

```bash
build/nexus_benchmark \
  --limit-mb 256 --scenario 256MiB-sequential \
  --iterations 4096 --pages 4096 --pool 256 \
  --db benchmarks/workloads/bench_256.db \
  --out benchmarks/results/bench_256MiB_sequential.txt
```

`--help` lists every knob. All knobs are echoed into the result so a run is
self-describing and reproducible.

## Expected output files

Under `benchmarks/results/`:

- `env.<utc>.txt` — machine/environment capture (uname, CPU count, gcc/g++,
  cgroup2 mount count, total memory) written by the runner.
- `bench_<MB>MiB_<order>.<utc>.txt` — one result per scenario. Format:

```
NexusDB Benchmark Result
========================
status: RUN (completed)
scenario: 256MiB-sequential
...
workload (measured)      steps_run / cache_hits / cache_misses / hit_ratio / latency
buffer_pool (measured)   start/end/min/max frames, grow/shrink/hold, failed_resizes
resources (measured)     samples, memory limit/current, pressure, cpu_percent
unavailable_metrics      explicitly-listed metrics that were NOT measured
environment_capture      the runner's environment block
```

A result file is written **only** when the engine-backed run completes. If the
binary is the Windows stub, or the engine is missing, the run refuses and writes
nothing.

## Status: RUN (completed)

The three scenarios have been executed on a supported Linux host (WSL2, kernel
6.18, GCC 15.2.0) with the integrated DB engine and cgroups v2, inside a
delegated cgroup scope. Real result files and the environment capture live in
`benchmarks/results/`:

- `env.20261018T155310Z.txt` — machine/environment capture
- `bench_512MiB_sequential.20261018T155310Z.txt`
- `bench_256MiB_sequential.20261018T155310Z.txt`
- `bench_128MiB_sequential.20261018T155310Z.txt`

Each result is `status: RUN (completed)` with measured resource/pool/latency
fields and an explicit `unavailable_metrics` block. See
`benchmarks/results/README.md` for the manifest.

## R4 measured results (WSL2, kernel 6.18, GCC 15.2.0, delegated cgroup)

Exact command (run per limit/order):

```bash
systemd-run --user --scope --quiet env NEXUS_CGROUP_ROOT=$DROOT \
  build/nexus_benchmark --limit-mb <128|256|512> \
    --scenario <N>MiB-<sequential|random> --iterations 4096 --pages 4096 \
    --pool 256 --sample-every 32 --dirty-every 8 \
    [--random --seed 1] --db /tmp/bench_<N>.db --out /tmp/bench_<N>.txt
```

All six runs `status: RUN (completed)`, `saw_limit=1`, `self_bound_to_cgroup=1`,
`failed_resizes: 0`. `pool` is `start→end` frames; `cache` is `hits/misses`.

| Limit | Order | pool start→end | grow | shrink | hold | cache hits/misses | mem.current last | pressure min–max | CPU avg |
|---|---|---|---|---|---|---|---|---|---|
| 512 MiB | sequential | 256→33280 | 129 | 0 | 0 | 0/4096 | 156 246 016 B | 0.037–0.291 | 56.5% |
| 256 MiB | sequential | 256→30208 | 117 | 0 | 12 | 0/4096 | 161 923 072 B | 0.138–0.603 | 57.0% |
| 128 MiB | sequential | 256→6912 | 26 | 0 | 103 | 0/4096 | 81 317 888 B | 0.404–0.607 | 60.4% |
| 512 MiB | random | 256→33280 | 129 | 0 | 0 | 1518/2578 | 206 860 288 B | 0.132–0.385 | 57.7% |
| 256 MiB | random | 256→18176 | 70 | 0 | 59 | 1518/2578 | 162 091 008 B | 0.326–0.604 | 59.4% |
| 128 MiB | random | 256→256 | 0 | 0 | 129 | 267/3829 | 104 357 888 B | 0.777–0.778 | 131.4% |

Observations (all directly from the measured columns above):

- Lower limits cap growth sooner: the 128 MiB random run sits at ~78% pressure,
  inside the hysteresis band, so the pool **holds** at 256 frames while the
  512/256 MiB runs grow. This is the policy responding to real cgroup pressure,
  not a scripted sequence.
- `shrink_requests` is 0 because pressure never crossed the 85% shrink threshold
  with an unbounded (growing) workload; shrink behaviour is instead proven
  deterministically by `tests/bridge/engine_adaptation_test.cpp`.
- `cpu_percent` may exceed 100% because the cgroup's `cpu.stat` aggregates all
  members/cores; CPU is **monitoring-only** and never drives resizing.

## Interpreting results

- `buffer_pool.grow_requests` / `shrink_requests` show whether pressure crossed
  the thresholds (`>85%` shrink, `<60%` grow, defined in
  `docs/container-runtime.md`). `hold_decisions` includes the hysteresis band,
  unlimited memory, and no-metrics samples.
- `failed_resizes > 0` means the engine rejected a resize (e.g. `kBufferFull`
  from pinned frames or allocation failure) — investigate before trusting the
  pool size trajectory.
- `cache_hit_ratio` is meaningful only when `steps_run > 0`; a read-only
  sequential walk over a pool smaller than `pages` produces misses by design.
- `pressure` and `memory_current` are only meaningful when `saw_limit=1` and the
  process is actually in the cgroup (`self_bound_to_cgroup=1`).
- `cpu_percent` requires two CPU-capable samples (`have_cpu=1`).

## Layout

```
include/benchmark/harness.h    harness API + honesty contract
src/benchmark/harness.c        C11 orchestration + result writer
src/benchmark/engine_workload.cpp   DB engine workload provider (Linux)
benchmarks/nexus_benchmark.c   CLI entry point
tests/benchmark/harness_test.c Windows-runnable self-test (synthetic; NOT a benchmark)
benchmarks/run_benchmarks.sh   three-scenario runner + env capture
benchmarks/workloads/          per-scenario database files (generated)
benchmarks/results/            captured results + environment (generated)
```
