#!/usr/bin/env bash
# NexusDB — cgroups v2 configuration script
#
# Configures delegation and permissions for the nexusdb cgroup hierarchy:
#   /sys/fs/cgroup/nexusdb
#
# Must be run with sudo once per boot.
set -euo pipefail

echo "=== NexusDB cgroups v2 Setup ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "[ERROR] This script must be run as root (use sudo)."
    exit 1
fi

CGROUP_ROOT="/sys/fs/cgroup"
NEXUS_CGROUP="$CGROUP_ROOT/nexusdb"
TARGET_USER="${SUDO_USER:-$USER}"

# 1. Verify cgroup v2
if ! mount | grep -q "type cgroup2"; then
    echo "[ERROR] cgroup v2 is not mounted at $CGROUP_ROOT. NexusDB requires cgroups v2."
    exit 1
fi

# 2. Enable memory and cpu controllers in root cgroup
echo "[*] Enabling memory and cpu controllers in root cgroup..."
if [ -f "$CGROUP_ROOT/cgroup.subtree_control" ]; then
    for controller in "+memory" "+cpu"; do
        if ! grep -q "${controller#+}" "$CGROUP_ROOT/cgroup.subtree_control" 2>/dev/null; then
            echo "$controller" > "$CGROUP_ROOT/cgroup.subtree_control" 2>/dev/null || true
        fi
    done
fi

# 3. Create nexusdb subcgroup
echo "[*] Initializing $NEXUS_CGROUP..."
mkdir -p "$NEXUS_CGROUP"

# 4. Enable controllers in nexusdb subcgroup for container delegation
if [ -f "$NEXUS_CGROUP/cgroup.subtree_control" ]; then
    echo "+memory +cpu" > "$NEXUS_CGROUP/cgroup.subtree_control" 2>/dev/null || true
fi

# 5. Delegate permissions to the non-root developer user
echo "[*] Delegating ownership to user '$TARGET_USER'..."
chown -R "$TARGET_USER:$TARGET_USER" "$NEXUS_CGROUP"
chmod 775 "$NEXUS_CGROUP"

echo "[+] NexusDB cgroup configured at $NEXUS_CGROUP"
echo "=== cgroup v2 setup successful! ==="
