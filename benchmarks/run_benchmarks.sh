#!/usr/bin/env bash
# NexusDB — benchmark runner (Rehan: Bridge & Observability).
#
# Runs the three required memory-limit scenarios (512, 256, 128 MiB) against the
# real database engine, capturing the exact command and machine/environment
# details alongside each result. Requires Linux with cgroups v2 and a build with
# the DB engine.
#
# Usage:
#   ./benchmarks/run_benchmarks.sh
#
# Outputs (under benchmarks/results/):
#   env.<utc>.txt                       machine/environment capture
#   bench_<MB>MiB_<order>.<utc>.txt     one result per scenario
#
# Nothing here invents data: a scenario that fails to run leaves no result file.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/nexus_benchmark"
RESULTS="$ROOT/benchmarks/results"
WORKLOADS="$ROOT/benchmarks/workloads"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"

if [ "$(uname -s)" != "Linux" ]; then
    echo "[ERROR] Benchmarks require Linux with cgroups v2 (Ubuntu 22.04 / WSL2)." >&2
    echo "        This host is: $(uname -s). Refusing to run." >&2
    exit 1
fi

mkdir -p "$RESULTS" "$WORKLOADS" "$ROOT/data"

if ! mount | grep -q "type cgroup2"; then
    echo "[WARNING] cgroups v2 is not mounted; pressure/limit readings may be unavailable." >&2
    echo "          Run: sudo ./scripts/setup_cgroups.sh" >&2
fi

if [ ! -x "$BIN" ]; then
    echo "[*] Building the benchmark (make benchmark)..."
    make -C "$ROOT" benchmark
fi

ENVFILE="$RESULTS/env.$STAMP.txt"
{
    echo "captured_at_utc: $STAMP"
    echo "uname: $(uname -a)"
    echo "nproc: $(nproc 2>/dev/null || echo unknown)"
    echo "gcc: $(gcc --version 2>/dev/null | head -n1 || echo unknown)"
    echo "g++: $(g++ --version 2>/dev/null | head -n1 || echo unknown)"
    echo "cgroup2_mounts: $(mount | grep -c 'type cgroup2' || true)"
    echo "mem_total: $(grep MemTotal /proc/meminfo 2>/dev/null || echo unknown)"
} | tee "$ENVFILE"
echo

run_scenario() {
    local mb="$1"
    local order="$2"
    local out="$RESULTS/bench_${mb}MiB_${order}.$STAMP.txt"
    local cmd=("$BIN"
               --limit-mb "$mb"
               --scenario "${mb}MiB-${order}"
               --iterations 4096
               --pages 4096
               --pool 256
               --sample-every 32
               --dirty-every 8
               --db "$WORKLOADS/bench_${mb}.db"
               --out "$out")

    if [ "$order" = "random" ]; then
        cmd+=(--random --seed 1)
    fi

    rm -f "$WORKLOADS/bench_${mb}.db"
    echo "[*] ${mb} MiB (${order})"
    echo "    cmd: ${cmd[*]}"
    "${cmd[@]}"
    echo "    out: $out"
    echo
}

for mb in 512 256 128; do
    run_scenario "$mb" sequential
done

echo "[+] Benchmarks complete. Results in: $RESULTS"
echo "    environment capture: $ENVFILE"
