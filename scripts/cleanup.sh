#!/usr/bin/env bash
# NexusDB — Teardown & cleanup script
#
# Terminates stray placeholder child processes, cleans up cgroup directories,
# and removes temporary test databases and logs.
set -euo pipefail

echo "=== NexusDB Teardown & Cleanup ==="

# 1. Terminate stray processes
echo "[*] Cleaning up orphaned test processes..."
pkill -f "nexusdb" 2>/dev/null || true
pkill -f "nexus_container_test" 2>/dev/null || true

# 2. Cleanup cgroups
NEXUS_CGROUP="/sys/fs/cgroup/nexusdb"
if [ -d "$NEXUS_CGROUP" ]; then
    echo "[*] Cleaning up child cgroups under $NEXUS_CGROUP..."
    find "$NEXUS_CGROUP" -mindepth 1 -maxdepth 1 -type d | while read -r dir; do
        if [ -f "$dir/cgroup.procs" ]; then
            # Move any lingering processes back to root or kill them
            while read -r pid; do
                kill -9 "$pid" 2>/dev/null || true
            done < "$dir/cgroup.procs"
        fi
        rmdir "$dir" 2>/dev/null || true
    done
fi

# 3. Clean temporary files
echo "[*] Removing temporary database files and build logs..."
rm -f /tmp/nexusdb_*.db /tmp/nexusdb_*.log
rm -rf /tmp/nexus_cgroup

echo "[+] Cleanup complete!"
