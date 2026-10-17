#!/usr/bin/env bash
# NexusDB — Environment setup script
#
# Verifies system prerequisites, installs dependencies, and initializes
# project runtime directories.
set -euo pipefail

echo "=== NexusDB Environment Setup ==="

# 1. Create project directories
mkdir -p build data logs benchmarks/workloads benchmarks/results

echo "[+] Created directories: build/, data/, logs/, benchmarks/"

# 2. Check toolchain dependencies
echo "[*] Checking development toolchain..."
MISSING_TOOLS=()
for cmd in gcc g++ cmake make; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        MISSING_TOOLS+=("$cmd")
    fi
done

if [ ${#MISSING_TOOLS[@]} -ne 0 ]; then
    echo "[!] Missing tools: ${MISSING_TOOLS[*]}"
    if command -v apt-get >/dev/null 2>&1; then
        echo "[*] Installing missing dependencies via apt..."
        sudo apt-get update
        sudo apt-get install -y build-essential cmake libncurses-dev
    else
        echo "[ERROR] Please install: ${MISSING_TOOLS[*]}"
        exit 1
    fi
else
    echo "[+] Toolchain (gcc, g++, cmake, make) is ready."
fi

# 3. Check cgroups v2
if mount | grep -q cgroup2; then
    echo "[+] cgroups v2 mount verified on /sys/fs/cgroup"
else
    echo "[WARNING] cgroups v2 not detected! Ensure kernel supports cgroups v2 (Ubuntu 22.04 / WSL2)."
fi

echo "=== Setup complete! Run sudo ./scripts/setup_cgroups.sh to configure cgroup permissions. ==="
