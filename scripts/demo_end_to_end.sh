#!/usr/bin/env bash
# NexusDB — End-to-end demo script
#
# Builds the project, runs the full test suite, and drives the complete demo:
#   - database CRUD: init, table create, insert, update, delete, scan
#   - container lifecycle: create, start, stop, delete
#   - pipeline: container -> cgroup -> OS metrics/pressure -> DB bridge -> dashboard
#
# Real kernel enforcement (cgroup limits, OOM kills, CPU throttling) needs a
# writable cgroup v2 subtree. When run from a plain shell the demo binaries
# honestly report ENV-LIMITED and exit 0; the same binaries exercise the real
# kernel when run inside a delegated systemd user scope. This script runs both
# and skips the delegated half automatically if no user scope is available.
#
# Usage:
#   bash scripts/demo_end_to_end.sh [build_dir]
#
# Environment:
#   BUILD_DIR           build directory (default: build-linux-clean)
#   NEXUS_CGROUP_ROOT   explicit writable cgroup root for delegated runs
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${1:-${BUILD_DIR:-build-linux-clean}}"

section() {
    echo
    echo "============================================================"
    echo "  $*"
    echo "============================================================"
}

fail=0
run() {
    local desc="$1"
    shift
    echo "--- $desc ---"
    if "$@"; then
        echo "    OK"
    else
        local rc=$?
        echo "    FAILED (exit $rc)"
        fail=1
    fi
}

delegated_ok=0
if command -v systemd-run >/dev/null 2>&1 &&
    systemd-run --user --scope --quiet -- true >/dev/null 2>&1; then
    delegated_ok=1
fi

run_delegated() {
    local desc="$1"
    shift
    if [ "$delegated_ok" -ne 1 ]; then
        echo "--- $desc (SKIPPED: systemd user scope unavailable) ---"
        return 0
    fi
    if [ -n "${NEXUS_CGROUP_ROOT:-}" ]; then
        run "$desc" systemd-run --user --scope --quiet -- env "NEXUS_CGROUP_ROOT=$NEXUS_CGROUP_ROOT" "$@"
    else
        run "$desc" systemd-run --user --scope --quiet -- "$@"
    fi
}

section "0. CONFIGURE + BUILD"
run "cmake configure" cmake -S . -B "$BUILD_DIR"
run "cmake build" cmake --build "$BUILD_DIR" --clean-first --parallel

section "1. FULL TEST SUITE (plain shell)"
run "ctest (plain)" ctest --test-dir "$BUILD_DIR" --output-on-failure

section "2. THE THREE CORE LIFECYCLE TESTS"
run "container_lifecycle / container_engine_demo / dashboard_demo" \
    ctest --test-dir "$BUILD_DIR" --output-on-failure \
    -R 'container_lifecycle|container_engine_demo|dashboard_demo'

section "3. CONTAINER ENGINE DEMO (lifecycle state machine)"
run "container_engine_demo (plain)" \
    ./"$BUILD_DIR"/container_engine_demo
run_delegated "container_engine_demo (delegated)" \
    ./"$BUILD_DIR"/container_engine_demo

section "4. LIVE DASHBOARD DEMO (container -> bridge -> policy -> text)"
run "dashboard_demo (plain)" \
    ./"$BUILD_DIR"/nexus_dashboard_demo nexusdash 512 3
run_delegated "dashboard_demo (delegated)" \
    ./"$BUILD_DIR"/nexus_dashboard_demo nexusdash 512 3

section "5. ENGINE-BACKED DASHBOARD DEMO (real buffer pool + adaptation)"
run "dashboard_engine (plain)" \
    ./"$BUILD_DIR"/nexus_dashboard_engine nexusdb-demo 256 3
run_delegated "dashboard_engine (delegated)" \
    ./"$BUILD_DIR"/nexus_dashboard_engine nexusdb-demo 256 3

section "6. DATABASE: INIT, TABLE, INSERT, UPDATE, DELETE, SCAN"
CLI=./"$BUILD_DIR"/nexusdb
DEMO_DATA_ROOT="$(mktemp -d)/nexusdb_data"
export NEXUSDB_DATA_ROOT="$DEMO_DATA_ROOT"
echo "[*] data root: $DEMO_DATA_ROOT"
run "db init demo"            "$CLI" db init demo
run "table create demo users" "$CLI" table create demo users
run "table list demo"         "$CLI" table list demo

echo "--- insert sample record (text 'Hello from NexusDB') ---"
INSERT_OUT=$("$CLI" insert demo users 'Hello from NexusDB' 2>&1) || fail=1
echo "    $INSERT_OUT"
PAGE=$(echo "$INSERT_OUT" | sed -n 's/.*page=\([0-9]*\).*/\1/p')
SLOT=$(echo "$INSERT_OUT" | sed -n 's/.*slot=\([0-9]*\).*/\1/p')
if [ -n "$PAGE" ] && [ -n "$SLOT" ]; then
    echo "    OK (record id page=$PAGE slot=$SLOT)"
else
    echo "    FAILED (could not parse page/slot from insert)"
    fail=1
fi

echo "--- get after insert (expect hex=48656c6c6f2066726f6d204e657875734442) ---"
"$CLI" get demo users "$PAGE" "$SLOT" || fail=1
echo "--- scan table (expect one record) ---"
"$CLI" scan demo users || fail=1

run "update record to text 'Updated record'" \
    "$CLI" update demo users "$PAGE" "$SLOT" 'Updated record'
echo "--- get after update (expect hex=55706461746564207265636f7264) ---"
"$CLI" get demo users "$PAGE" "$SLOT" || fail=1

run "delete record" "$CLI" delete demo users "$PAGE" "$SLOT"
run "scan after delete (expect: no records)" "$CLI" scan demo users
echo "--- get deleted record (expect exit 4) ---"
rc=0
"$CLI" get demo users "$PAGE" "$SLOT" || rc=$?
if [ "$rc" -eq 4 ]; then
    echo "    OK (exit 4 as expected)"
else
    echo "    FAILED (expected exit 4, got $rc)"
    fail=1
fi

section "7. FULL END-TO-END DEMO WITH REAL KERNEL ENFORCEMENT"
run_delegated "ctest (delegated)" \
    ctest --test-dir "$BUILD_DIR" --output-on-failure
run_delegated "dashboard_engine (delegated, full pipeline)" \
    ./"$BUILD_DIR"/nexus_dashboard_engine nexusdb-demo 256 3

section "8. CLEANUP"
find /sys/fs/cgroup/user.slice/user-"$(id -u)".slice/user@"$(id -u)".service \
    -depth -type d -name 'nexus*' -exec rmdir {} + 2>/dev/null || true
rm -rf /tmp/nexus_lc_* /tmp/nexus_cgtest_* /tmp/nexus_eng_* 2>/dev/null || true
if [ -n "${DEMO_DATA_ROOT:-}" ]; then
    rm -rf "$DEMO_DATA_ROOT" 2>/dev/null || true
    rmdir "$(dirname "$DEMO_DATA_ROOT")" 2>/dev/null || true
fi
echo "[*] cleanup done"

echo
if [ "$fail" -eq 0 ]; then
    echo "=== END-TO-END DEMO PASSED ==="
else
    echo "=== END-TO-END DEMO FAILED ==="
fi
exit "$fail"
