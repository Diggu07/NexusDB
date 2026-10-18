/*
 * NexusDB — OS–DB Bridge: resource monitoring implementation (Rehan).
 * See include/bridge/resource_monitor.h for the contract.
 *
 * The heavy lifting (opening, parsing, overflow checks for memory.max, the
 * "max" -> unlimited convention, honesty on missing files) lives in the
 * existing cgroup controller; this module only adds per-field availability
 * flags, an independent CPU-availability probe, the container lookup, and the
 * CPU-rate calculation.
 */
#include "bridge/resource_monitor.h"

#include <stdio.h>
#include <string.h>

#include "container/cgroup.h"

/* Join a cgroup directory and a child file name, matching the separator
 * convention used by the container/cgroup layers (backslash on Windows). */
static void child_path(char *buf, size_t len, const char *dir, const char *file)
{
#ifdef _WIN32
    snprintf(buf, len, "%s\\%s", dir, file);
#else
    snprintf(buf, len, "%s/%s", dir, file);
#endif
}

/* 1 when <dir>/cpu.stat can be opened for reading. The cgroup controller
 * treats cpu.stat as optional (missing -> usage 0); we surface its presence
 * explicitly so the dashboard can show "unavailable" instead of a fake 0. */
static int cpu_stat_present(const char *dir)
{
    char path[NEXUS_PATH_MAX + 32];
    FILE *f;

    child_path(path, sizeof(path), dir, "cpu.stat");
    f = fopen(path, "r");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

int nexus_bridge_read_cgroup(const char *cgroup_dir,
                             nexus_resource_snapshot_t *out)
{
    nexus_cgroup_metrics_t m;
    int rc;

    if (cgroup_dir == NULL || cgroup_dir[0] == '\0' || out == NULL)
        return NEXUS_ERR_INVALID;

    memset(out, 0, sizeof(*out));

    rc = cgroup_read_metrics(cgroup_dir, &m);
    if (rc != NEXUS_OK)
        return rc; /* out stays zeroed: no fabricated memory reading */

    out->valid = 1;
    out->has_memory = 1;
    out->memory_current = m.memory_current;
    out->memory_max = m.memory_max;
    out->has_limit = (m.memory_max > 0);
    /* CPU presence is probed independently so a missing cpu.stat does not
     * invalidate an otherwise-good memory snapshot. */
    out->has_cpu = cpu_stat_present(cgroup_dir);
    out->cpu_usage_usec = out->has_cpu ? m.cpu_usage_usec : 0;
    out->memory_pressure = m.memory_pressure;
    return NEXUS_OK;
}

int nexus_bridge_read_container(const char *container_name,
                                nexus_resource_snapshot_t *out)
{
    const nexus_container_t *c;

    if (container_name == NULL || container_name[0] == '\0' || out == NULL)
        return NEXUS_ERR_INVALID;

    c = container_get(container_name);
    if (c == NULL)
        return NEXUS_ERR_NOT_FOUND;
    if (c->state == NEXUS_DELETED)
        return NEXUS_ERR_STATE;

    return nexus_bridge_read_cgroup(c->cgroup_path, out);
}

double nexus_bridge_cpu_percent(const nexus_resource_snapshot_t *prev,
                                const nexus_resource_snapshot_t *cur,
                                long long elapsed_ms)
{
    unsigned long delta_usec;

    if (prev == NULL || cur == NULL)
        return -1.0;
    if (!prev->valid || !cur->valid || !prev->has_cpu || !cur->has_cpu)
        return -1.0;
    if (elapsed_ms <= 0)
        return -1.0;
    if (cur->cpu_usage_usec < prev->cpu_usage_usec)
        return -1.0; /* counter reset: a rate would be meaningless */

    delta_usec = cur->cpu_usage_usec - prev->cpu_usage_usec;
    /* delta microseconds / (elapsed milliseconds * 1000) * 100 = percent. */
    return (double)delta_usec / ((double)elapsed_ms * 1000.0) * 100.0;
}

const char *nexus_bridge_strerror(int rc)
{
    switch (rc) {
    case NEXUS_OK:            return "ok";
    case NEXUS_ERR_INVALID:   return "invalid argument";
    case NEXUS_ERR_NOT_FOUND: return "container not found";
    case NEXUS_ERR_EXISTS:    return "container already exists";
    case NEXUS_ERR_STATE:     return "illegal state for this operation";
    case NEXUS_ERR_NOMEM:     return "container registry full";
    case NEXUS_ERR_PROCESS:   return "resource metrics unavailable";
    default:                  return "unknown error";
    }
}
