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
