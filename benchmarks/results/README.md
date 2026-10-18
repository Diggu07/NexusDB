# Benchmark results — status: NOT RUN

No benchmark has been executed yet. Results are produced only by a successful
`status: RUN (completed)` run on Linux with the integrated DB engine and
cgroups v2. The Windows dev machine can build the harness and run its self-test,
but it cannot produce real benchmark numbers.

## Scenario manifest

| Scenario | Limit | Order | Status | Result file |
|---|---|---|---|---|
| 512MiB-sequential | 512 MiB | sequential | **NOT RUN** | `bench_512MiB_sequential.<utc>.txt` |
| 256MiB-sequential | 256 MiB | sequential | **NOT RUN** | `bench_256MiB_sequential.<utc>.txt` |
| 128MiB-sequential | 128 MiB | sequential | **NOT RUN** | `bench_128MiB_sequential.<utc>.txt` |

## How a run marks itself RUN

Each successful run writes `status: RUN (completed)` (or `status: STUB/refused`
if the engine is absent) at the top of its result file, plus a matching
`env.<utc>.txt` environment capture. Do not cite numbers from this directory
until those files exist.

Run the suite with:

```bash
./benchmarks/run_benchmarks.sh
```
