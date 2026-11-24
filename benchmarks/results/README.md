# Benchmark results - status: RUN (completed)

The three required scenarios were re-run on 2026-11-23 (container clock) on
Linux (WSL2, kernel 6.18.58.1, GCC/g++ 15.2.0, cgroups v2) with the integrated
DB engine, in the delegated `/sys/fs/cgroup/nexusdb` subtree. Every scenario
finished with `status: RUN (completed)`.

## Scenario manifest

| Scenario | Limit | Order | Status | Result file |
|---|---|---|---|---|
| 512MiB-sequential | 512 MiB | sequential | **RUN (completed)** | `bench_512MiB_sequential.20261123T195229Z.txt` |
| 256MiB-sequential | 256 MiB | sequential | **RUN (completed)** | `bench_256MiB_sequential.20261123T195229Z.txt` |
| 128MiB-sequential | 128 MiB | sequential | **RUN (completed)** | `bench_128MiB_sequential.20261123T195229Z.txt` |

Environment capture: `env.20261123T195229Z.txt`.

## Analysis

All three scenarios completed all 4096 steps with zero fetch errors and zero
failed resizes. The sequential unique-page workload reports
`cache_hit_ratio: 0.0000`; that is the measured `Contains()`-level result, not a
claim about engine-reported cache counters (which this harness does not sample).
The 512 MiB and 256 MiB runs grew from 256 to 33280 pool frames with no hold
decisions; the 128 MiB run stopped at 18944 frames with 56 hold decisions, the
highest observed pressure (`0.6112`), and the lowest average step latency
(`4.6754 ms`). Because these are short single-process runs, the latency
differences should not be read as a causal limit-vs-performance result; the
defensible observations are completion, real cgroup binding, bounded pool
growth, and no resize failures.

## How a run marks itself RUN

Each successful run writes `status: RUN (completed)` (or `status: STUB/refused`
if the engine is absent) at the top of its result file, plus a matching
`env.<utc>.txt` environment capture. Metrics that were not measured are listed
explicitly under `unavailable_metrics` and never fabricated.

Re-run the suite with:

```bash
BUILD_DIR=build-wsl ./benchmarks/run_benchmarks.sh
```
