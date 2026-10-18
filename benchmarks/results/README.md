# Benchmark results - status: RUN (completed)

The three required scenarios were executed on 2026-10-18 on Linux (WSL2, kernel
6.18.54.1, GCC/g++ 15.2.0, cgroups v2) with the integrated DB engine, inside a
delegated cgroup scope. Every scenario finished with `status: RUN (completed)`.

## Scenario manifest

| Scenario | Limit | Order | Status | Result file |
|---|---|---|---|---|
| 512MiB-sequential | 512 MiB | sequential | **RUN (completed)** | `bench_512MiB_sequential.20261018T155310Z.txt` |
| 256MiB-sequential | 256 MiB | sequential | **RUN (completed)** | `bench_256MiB_sequential.20261018T155310Z.txt` |
| 128MiB-sequential | 128 MiB | sequential | **RUN (completed)** | `bench_128MiB_sequential.20261018T155310Z.txt` |

Environment capture: `env.20261018T155310Z.txt`.

## How a run marks itself RUN

Each successful run writes `status: RUN (completed)` (or `status: STUB/refused`
if the engine is absent) at the top of its result file, plus a matching
`env.<utc>.txt` environment capture. Metrics that were not measured are listed
explicitly under `unavailable_metrics` and never fabricated.

Re-run the suite with:

```bash
./benchmarks/run_benchmarks.sh
```
