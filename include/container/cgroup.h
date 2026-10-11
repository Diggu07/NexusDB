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

typedef struct nexus_cgroup_metrics {
    unsigned long memory_current; /* current memory in bytes */
    unsigned long memory_max;     /* memory limit in bytes (0 if unlimited) */
    unsigned long cpu_usage_usec; /* total CPU usage in microseconds */
    double memory_pressure;       /* memory_current / memory_max (0.0 to 1.0+) */
} nexus_cgroup_metrics_t;

/* Initialize root cgroup directory and enable controllers (+memory +cpu).
 * root_override can be NULL (defaults to /sys/fs/cgroup/nexusdb or NEXUS_CGROUP_ROOT env). */
int cgroup_init(const char *root_override);

/* Create container cgroup subdirectory: <root>/<name> */
int cgroup_create(const char *name, char *out_path, size_t out_path_len);

/* Configure memory and CPU resource limits on container cgroup */
int cgroup_set_limits(const char *cgroup_path, unsigned long memory_bytes, unsigned int cpu_percent);

/* Attach process PID to container cgroup (cgroup.procs) */
int cgroup_attach(const char *cgroup_path, long pid);

/* Read real-time resource metrics from cgroup v2 */
int cgroup_read_metrics(const char *cgroup_path, nexus_cgroup_metrics_t *metrics);

/* Destroy container cgroup on deletion */
int cgroup_destroy(const char *cgroup_path);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_CGROUP_H */
