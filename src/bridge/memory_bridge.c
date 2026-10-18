/*
 * NexusDB — OS-to-DB Bridge: memory-telemetry relay implementation.
 * See include/bridge/memory_bridge.h for scope and ownership.
 *
 * Consumer-only: every read goes through container_metrics(); this file
 * performs no cgroup file I/O of its own, writes no limits, spawns no
 * processes, and owns no background threads.
 */
#include "bridge/memory_bridge.h"

#include <stdio.h>

int memory_bridge_query(const char *name, nexus_cgroup_metrics_t *out)
{
    if (out == NULL)
        return NEXUS_ERR_INVALID;
    /* container_metrics() owns name validation (charset/length), the
     * registry lookup, and the cgroup file reads. Delegating keeps one
     * implementation of limit/telemetry logic in the container layer. */
    return container_metrics(name, out);
}

int memory_bridge_format(const nexus_cgroup_metrics_t *metrics,
                         char *buf, size_t buflen)
{
    int n;

    if (metrics == NULL || buf == NULL || buflen == 0)
        return NEXUS_ERR_INVALID;

    if (metrics->memory_max > 0) {
        n = snprintf(buf, buflen,
                     "Memory current: %lu bytes | max: %lu bytes | pressure: %.4f",
                     metrics->memory_current,
                     metrics->memory_max,
                     metrics->memory_pressure);
    } else {
        n = snprintf(buf, buflen,
                     "Memory current: %lu bytes | max: unlimited | pressure: %.4f",
                     metrics->memory_current,
                     metrics->memory_pressure);
    }
    if (n < 0 || (size_t)n >= buflen)
        return NEXUS_ERR_INVALID; /* caller buffer too small */
    return NEXUS_OK;
}

int memory_bridge_pressure_state(const nexus_cgroup_metrics_t *metrics)
{
    if (metrics == NULL)
        return MEMORY_BRIDGE_INVALID;
    if (metrics->memory_max == 0)
        return MEMORY_BRIDGE_NORMAL; /* unlimited: pressure defined as 0.0 */
    if (metrics->memory_pressure > NEXUS_BRIDGE_HIGH_PRESSURE)
        return MEMORY_BRIDGE_HIGH;
    if (metrics->memory_pressure < NEXUS_BRIDGE_LOW_PRESSURE)
        return MEMORY_BRIDGE_LOW;
    return MEMORY_BRIDGE_NORMAL;
}
