/*
 * NexusDB — OS-to-DB Bridge: memory-telemetry relay (Phase 2 demo).
 *
 * Narrowly scoped, read-only milestone. This module is the ONLY bridge
 * code owned by the container-runtime side; Rehan owns the adaptation
 * policy, buffer-pool resizing, dashboard, and benchmarks.
 *
 * Responsibility (and nothing else):
 *   - Read real cgroups v2 memory information (memory.current,
 *     memory.max) for an EXISTING container via the existing
 *     container_metrics() interface.
 *   - Return it to the caller and render it as display text for the
 *     existing CLI / database-side output style.
 *
 * Explicitly OUT OF SCOPE here (Rehan's): automatic buffer-pool
 * resizing, memory-pressure policy actions, monitoring loops, dashboard,
 * cache-hit tracking, benchmarks, sockets, or any new IPC protocol.
 * pressure_state() below is a read-only display hint using the
 * documented demo thresholds; it takes no action and changes no limits.
 *
 * Dependency direction: bridge -> container only. No container,
 * namespace, cgroup, or DB-engine file includes this header, so there
 * is no circular dependency and no second resource allocator: limits
 * are configured and enforced exclusively by the container layer.
 */
#ifndef NEXUS_MEMORY_BRIDGE_H
#define NEXUS_MEMORY_BRIDGE_H

#include <stddef.h>

#include "container/container.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display-hint thresholds (match docs/container-runtime.md demo values).
 * Read-only: no resizing or policy action is taken at any threshold. */
#define NEXUS_BRIDGE_HIGH_PRESSURE 0.85
#define NEXUS_BRIDGE_LOW_PRESSURE 0.60

/* Read-only pressure classification for display purposes only. */
typedef enum memory_bridge_state {
    MEMORY_BRIDGE_INVALID = -2, /* NULL metrics pointer */
    MEMORY_BRIDGE_LOW = -1,     /* pressure < 0.60: headroom hint */
    MEMORY_BRIDGE_NORMAL = 0,    /* 0.60 .. 0.85, or unlimited ("max") */
    MEMORY_BRIDGE_HIGH = 1      /* pressure > 0.85: pressure hint */
} memory_bridge_state_t;

/* Read memory telemetry for an existing container.
 * Thin, compatible wrapper over container_metrics(): validates out and
 * delegates all lookup/validation/reading to the container layer, so the
 * bridge never duplicates limit logic. Returns NEXUS_OK on success,
 * NEXUS_ERR_INVALID on NULL args, otherwise the container layer code
 * (NOT_FOUND, PROCESS, STATE). Never writes limits. */
int memory_bridge_query(const char *name, nexus_cgroup_metrics_t *out);

/* Render telemetry as one display line for the CLI/demo output style:
 * "Memory current: <cur> bytes | max: <max|unlimited> | pressure: <p>".
 * Returns NEXUS_OK, or NEXUS_ERR_INVALID on NULL args or when buflen
 * cannot hold the line (mirrors container_status truncation handling). */
int memory_bridge_format(const nexus_cgroup_metrics_t *metrics,
                         char *buf, size_t buflen);

/* Classify metrics into a display hint. No side effects: never resizes,
 * never writes limits, never starts/stops containers. Returns the
 * memory_bridge_state_t value, or MEMORY_BRIDGE_INVALID on NULL input.
 * Unlimited ("max", memory_max == 0) reports NORMAL. */
int memory_bridge_pressure_state(const nexus_cgroup_metrics_t *metrics);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_MEMORY_BRIDGE_H */
