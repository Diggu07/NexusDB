/*
 * NexusDB — OS–DB Bridge: resource monitoring (Rehan: Bridge & Observability)
 *
 * Reads real cgroup v2 resource statistics for one container so that the
 * adaptation policy and the dashboard can make honest decisions.
 *
 * Design boundaries:
 *   - This module OWNS only the bridge layer. It does not touch the database
 *     engine and does not resize anything; it reports what the kernel says.
 *   - The container's cgroup directory is obtained from the container runtime
 *     (container_get()->cgroup_path). No path is ever hardcoded.
 *   - Reading is delegated to the existing cgroup controller
 *     (cgroup_read_metrics, Ayush), which already enforces the "required
 *     memory files or a real error, never fake zero" contract.
 *
 * Honesty contract:
 *   - Every field carries an explicit availability flag, so "unavailable" is
 *     distinguishable from a genuine zero.
 *   - A snapshot is `valid` only when BOTH memory.current and memory.max were
 *     read and parsed; otherwise the snapshot is zeroed with valid == 0 and
 *     the caller gets a real error code.
 *   - CPU is optional: a missing/unreadable cpu.stat yields has_cpu == 0 and
 *     cpu_usage_usec == 0 (never a fabricated rate).
 *
 * Platform / representation notes (reported honestly):
 *   - On Linux the files are kernel-owned; on the Windows dev shim they are
 *     mock files, so a snapshot here proves parsing, not kernel enforcement.
 *   - Values use `unsigned long`, matching the existing container metrics
 *     struct. On Windows (LLP64) that is 32-bit, so a value above ~4 GiB would
 *     saturate; the benchmarked limits (<= 512 MB) are well inside range.
 */
#ifndef NEXUS_BRIDGE_RESOURCE_MONITOR_H
#define NEXUS_BRIDGE_RESOURCE_MONITOR_H

#include "container/container.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A point-in-time resource reading for one container cgroup.
 *
 *   valid          memory.current AND memory.max were read and parsed
 *   has_memory     memory_current is a real reading (implies valid)
 *   has_limit      memory_max is a finite limit (0 => unlimited/"max")
 *   has_cpu        cpu.stat was readable, so cpu_usage_usec is meaningful
 *   memory_current bytes currently charged to the cgroup
 *   memory_max     limit in bytes; 0 means unlimited
 *   cpu_usage_usec cumulative CPU time consumed (microseconds)
 *   memory_pressure current/limit in [0, inf); 0.0 when unlimited/unavailable
 */
typedef struct nexus_resource_snapshot {
    int valid;
    int has_memory;
    int has_limit;
    int has_cpu;
    unsigned long memory_current;
    unsigned long memory_max;
    unsigned long cpu_usage_usec;
    double memory_pressure;
} nexus_resource_snapshot_t;

/*
 * Read a resource snapshot directly from a container's cgroup v2 directory.
 *
 * `cgroup_dir` is the directory returned by container_get()->cgroup_path; it
 * is never assumed. `out` is filled on success and zeroed on failure.
 *
 * Returns NEXUS_OK when memory metrics were read (out->valid == 1), or:
 *   NEXUS_ERR_INVALID   cgroup_dir is NULL/empty or out is NULL
 *   NEXUS_ERR_PROCESS   memory.current/memory.max missing, unreadable, or
 *                       malformed (out is zeroed; no value is fabricated)
 */
int nexus_bridge_read_cgroup(const char *cgroup_dir,
                             nexus_resource_snapshot_t *out);

/*
 * Resolve `container_name` through the container runtime and read its
 * cgroup snapshot. Returns NEXUS_ERR_INVALID for NULL/empty name or out,
 * NEXUS_ERR_NOT_FOUND for an unknown container, NEXUS_ERR_STATE for a
 * DELETED container, and otherwise the code from nexus_bridge_read_cgroup().
 */
int nexus_bridge_read_container(const char *container_name,
                                nexus_resource_snapshot_t *out);

/*
 * CPU utilization percentage between two snapshots taken `elapsed_ms`
 * milliseconds apart, derived from the cumulative cpu.stat usage_usec.
 * 100.0 means one fully-busy core over the interval.
 *
 * Returns a negative value when it cannot be computed honestly:
 *   - either snapshot is NULL, invalid, or lacks CPU data (has_cpu == 0)
 *   - elapsed_ms <= 0
 *   - the counter moved backwards (a reset), which would produce a bogus rate
 */
double nexus_bridge_cpu_percent(const nexus_resource_snapshot_t *prev,
                                const nexus_resource_snapshot_t *cur,
                                long long elapsed_ms);

/* Human-readable description of a bridge status code (never NULL). */
const char *nexus_bridge_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_BRIDGE_RESOURCE_MONITOR_H */
