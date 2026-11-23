/*
 * NexusDB — Container Engine: cgroups v2 interface (Ayush: Systems Engineer)
 *
 * Implements Linux cgroups v2 resource control:
 *   - memory.max / memory.current (buffer pool pressure signals)
 *   - cpu.max (CPU allocation control)
 *   - cgroup.procs (process membership)
 *
 * Provides live telemetry to Rehan's OS-DB Bridge and the metrics dashboard.
 */
#ifndef NEXUS_CGROUP_H
#define NEXUS_CGROUP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEXUS_CGROUP_DEFAULT_ROOT "/sys/fs/cgroup/nexusdb"

/* Standard cgroup v2 mount point. Used only to resolve a default root from the
 * running process's own cgroup when the hardcoded default is not usable. */
#define NEXUS_CGROUP_MOUNT "/sys/fs/cgroup"

typedef struct nexus_cgroup_metrics {
    unsigned long memory_current; /* current memory in bytes */
    unsigned long memory_max;     /* memory limit in bytes (0 if unlimited) */
    unsigned long cpu_usage_usec; /* total CPU usage in microseconds */
    double memory_pressure;       /* memory_current / memory_max (0.0 to 1.0+) */

    /* cpu.max: quota and period in microseconds. has_cpu_limit == 1 only when a
     * finite quota was parsed; cpu_quota_us == 0 means unlimited ("max"). A
     * missing cpu.max file leaves has_cpu_limit == 0 (unavailable, not zero). */
    int has_cpu_limit;
    unsigned long cpu_quota_us;
    unsigned long cpu_period_us;

    /* memory.pressure (PSI): "some avg10" as a percentage (0..100). Optional;
     * has_pressure == 0 when the file is missing/unsupported. */
    int has_pressure;
    double pressure_some_avg10;

    /* memory.events OOM counters. Optional; has_memory_events == 0 when the
     * file is missing/unsupported. */
    int has_memory_events;
    unsigned long memory_oom_events;
    unsigned long memory_oom_kill_events;
} nexus_cgroup_metrics_t;

/* Return the effective cgroup root used by this implementation: an explicit
 * cgroup_init() override, else $NEXUS_CGROUP_ROOT, else the platform default
 * (NEXUS_CGROUP_DEFAULT_ROOT on Linux, a %TEMP% shim on Windows). The pointer
 * is owned by the implementation and remains valid for the program lifetime;
 * call after cgroup_init() to observe a resolved override. */
const char *cgroup_root(void);

/* Human-readable detail for the most recent public cgroup operation failure:
 * the operation, the path involved, and the OS error (strerror). Empty after a
 * successful operation. Callers can append this to user-facing errors instead
 * of reporting only a generic "operation failed". The returned pointer is
 * static and valid until the next cgroup call. */
const char *cgroup_last_error(void);

/* Reset the detail string. Call before an operation whose failure may not come
 * from the cgroup layer, so stale details are never attached to the wrong
 * error. */
void cgroup_clear_error(void);

/* 1 when this process already lives in the cgroup subtree that owns the
 * effective root, so it can migrate a child into <root>/<name> without crossing
 * a cgroup the user does not own. 1 is also returned for an explicit non-kernel
 * (shim/test) root, where kernel delegation does not apply. 0 means the caller
 * must enter the delegated subtree first (on this WSL setup: `systemd-run
 * --user --scope`) or cgroup_attach will be denied by the kernel. */
int cgroup_self_in_delegated_root(void);

/* Initialize root cgroup directory and enable controllers (+memory +cpu).
 * root_override can be NULL (defaults to /sys/fs/cgroup/nexusdb or NEXUS_CGROUP_ROOT env).
 * Idempotent: creating an existing root is success. Enabling subtree_control
 * is best-effort (already-enabled or unprivileged hierarchies still return OK
 * when the root directory itself is usable). Returns NEXUS_ERR_INVALID on a
 * bad override path, NEXUS_ERR_PROCESS when the root cannot be created. */
int cgroup_init(const char *root_override);

/* Create container cgroup subdirectory: <root>/<name>.
 * Validates the name (rejects NULL/empty, path separators, "." and "..").
 * Ensures the root exists (EEXIST is success). Returns NEXUS_ERR_INVALID on
 * bad arguments or truncation, NEXUS_ERR_PROCESS when the directories cannot
 * be created. */
int cgroup_create(const char *name, char *out_path, size_t out_path_len);

/* Configure memory and CPU resource limits on container cgroup.
 * Returns NEXUS_ERR_INVALID on a bad path or cpu_percent > 100,
 * NEXUS_ERR_PROCESS when memory.max / cpu.max cannot be written
 * (a limit that was not applied is never reported as success). */
int cgroup_set_limits(const char *cgroup_path, unsigned long memory_bytes, unsigned int cpu_percent);

/* Attach process PID to container cgroup (cgroup.procs).
 * Writes the pid and then VERIFIES membership by re-reading cgroup.procs, so a
 * write that reported success but did not actually move the process (or a move
 * denied by the kernel's common-ancestor permission rule) can never be
 * mistaken for an enforced limit. Returns NEXUS_ERR_INVALID on a bad path or
 * pid <= 0, NEXUS_ERR_PROCESS when the pid cannot be written or is not present
 * afterwards. */
int cgroup_attach(const char *cgroup_path, long pid);

/* Count the pids listed in <cgroup_path>/cgroup.procs. A nonzero count means
 * the cgroup owns at least one process (the monitored workload is attached).
 * Returns -1 when the path is invalid or cgroup.procs cannot be read (so a
 * missing file is distinguishable from a genuinely empty cgroup). */
int cgroup_count_procs(const char *cgroup_path);

/* Read real-time resource metrics from cgroup v2.
 * memory.current and memory.max are REQUIRED: missing, unreadable, or
 * unparsable files return NEXUS_ERR_PROCESS instead of fake zero-pressure
 * readings. cpu.stat is optional (missing file yields cpu_usage_usec == 0).
 * A memory.max of "max" (unlimited) is valid and yields pressure 0.0. */
int cgroup_read_metrics(const char *cgroup_path, nexus_cgroup_metrics_t *metrics);

/* Destroy container cgroup on deletion.
 * Refuses non-empty cgroups (cgroup.procs holding pids) with NEXUS_ERR_STATE
 * so live processes are never orphaned by a rmdir. Missing cgroups are
 * success (idempotent). Returns NEXUS_ERR_PROCESS when removal fails. */
int cgroup_destroy(const char *cgroup_path);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_CGROUP_H */
