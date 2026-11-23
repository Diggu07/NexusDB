/*
 * NexusDB — Live dashboard: plain-text renderer (always built).
 * See include/dashboard/dashboard.h for the contract.
 */
#include "dashboard/dashboard.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define UNAVAILABLE "unavailable"

/* Small growable-by-none buffer writer: tracks usage and truncation. */
typedef struct sb {
    char *buf;
    size_t cap;
    size_t used;
    int overflow;
} sb_t;

static void sb_line(sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room;

    if (sb->overflow)
        return;

    room = sb->cap - sb->used;
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->used, room, fmt, ap);
    va_end(ap);
    if (n < 0) {
        sb->overflow = 1;
        return;
    }
    if ((size_t)n >= room) {
        sb->overflow = 1;
        sb->buf[sb->cap - 1] = '\0';
        return;
    }
    sb->used += (size_t)n;
}

static void put_bytes(sb_t *sb, unsigned long bytes)
{
    char tmp[48];
    if (nexus_format_bytes(bytes, tmp, sizeof(tmp)) < 0)
        sb_line(sb, "%s", UNAVAILABLE);
    else
        sb_line(sb, "%s", tmp);
}

/* cpu.max quota/period expressed as a core count ("2.00 cores"). */
static void put_cpu_limit(sb_t *sb, const nexus_resource_snapshot_t *r)
{
    if (r->has_cpu_limit && r->cpu_period_us > 0)
        sb_line(sb, "%.2f cores", (double)r->cpu_quota_us / (double)r->cpu_period_us);
    else
        sb_line(sb, "unlimited");
}

int nexus_dashboard_render_text(const nexus_dashboard_metrics_t *metrics,
                                char *buf, size_t len)
{
    sb_t sb;

    if (metrics == NULL || buf == NULL || len == 0)
        return NEXUS_ERR_INVALID;

    sb.buf = buf;
    sb.cap = len;
    sb.used = 0;
    sb.overflow = 0;
    buf[0] = '\0';

    sb_line(&sb, "NexusDB Dashboard\n");
    sb_line(&sb, "=================\n");
    sb_line(&sb, "Resource\n--------\n");

    if (metrics->has_resource) {
        sb_line(&sb, "Attached:         %s\n",
                metrics->resource.attached ? "yes" : "no (limits not enforced)");
        sb_line(&sb, "Memory used:      ");
        put_bytes(&sb, metrics->resource.memory_current);
        sb_line(&sb, "\nMemory limit:     ");
        if (metrics->resource.has_limit)
            put_bytes(&sb, metrics->resource.memory_max);
        else
            sb_line(&sb, "unlimited");
        sb_line(&sb, "\nMemory pressure:  %.1f%%\n",
                metrics->resource.memory_pressure * 100.0);
        sb_line(&sb, "CPU limit:        ");
        put_cpu_limit(&sb, &metrics->resource);
        sb_line(&sb, "\n");
        if (metrics->resource.has_pressure)
            sb_line(&sb, "Mem PSI (some):   %.2f%%\n",
                    metrics->resource.pressure_some_avg10);
        else
            sb_line(&sb, "Mem PSI (some):   %s\n", UNAVAILABLE);
        if (metrics->resource.has_events)
            sb_line(&sb, "OOM events:       %lu (kills %lu)\n",
                    metrics->resource.oom_events,
                    metrics->resource.oom_kill_events);
        else
            sb_line(&sb, "OOM events:       %s\n", UNAVAILABLE);
    } else {
        sb_line(&sb, "Attached:         %s\n", UNAVAILABLE);
        sb_line(&sb, "Memory used:      %s\n", UNAVAILABLE);
        sb_line(&sb, "Memory limit:     %s\n", UNAVAILABLE);
        sb_line(&sb, "Memory pressure:  %s\n", UNAVAILABLE);
        sb_line(&sb, "CPU limit:        %s\n", UNAVAILABLE);
        sb_line(&sb, "Mem PSI (some):   %s\n", UNAVAILABLE);
        sb_line(&sb, "OOM events:       %s\n", UNAVAILABLE);
    }

    if (metrics->has_cpu_percent)
        sb_line(&sb, "CPU:              %.1f%%\n", metrics->cpu_percent);
    else
        sb_line(&sb, "CPU:              %s\n", UNAVAILABLE);

    sb_line(&sb, "Database Engine\n---------------\n");

    if (metrics->engine.has_pool) {
        sb_line(&sb, "Buffer target:    %lu frames\n",
                metrics->engine.pool_target_frames);
        sb_line(&sb, "Buffer actual:    %lu frames\n",
                metrics->engine.pool_actual_frames);
        sb_line(&sb, "Resident frames:  %lu\n",
                metrics->engine.pool_resident_frames);
        sb_line(&sb, "Pinned frames:    %lu\n",
                metrics->engine.pool_pinned_frames);
    } else {
        sb_line(&sb, "Buffer target:    %s\n", UNAVAILABLE);
        sb_line(&sb, "Buffer actual:    %s\n", UNAVAILABLE);
        sb_line(&sb, "Resident frames:  %s\n", UNAVAILABLE);
        sb_line(&sb, "Pinned frames:    %s\n", UNAVAILABLE);
    }

    if (metrics->engine.has_cache_stats) {
        char ratio[32];
        sb_line(&sb, "Cache hits:       %lu\n", metrics->engine.cache_hits);
        sb_line(&sb, "Cache misses:     %lu\n", metrics->engine.cache_misses);
        if (nexus_format_hit_ratio(metrics->engine.cache_hits,
                                   metrics->engine.cache_misses,
                                   ratio, sizeof(ratio)) < 0)
            sb_line(&sb, "Cache hit ratio:  %s\n", UNAVAILABLE);
        else
            sb_line(&sb, "Cache hit ratio:  %s\n", ratio);
    } else {
        sb_line(&sb, "Cache hits:       %s\n", UNAVAILABLE);
        sb_line(&sb, "Cache misses:     %s\n", UNAVAILABLE);
        sb_line(&sb, "Cache hit ratio:  %s\n", UNAVAILABLE);
    }

    if (metrics->engine.has_latency) {
        char lat[32];
        if (nexus_format_latency_ms(metrics->engine.latency_ms, lat,
                                    sizeof(lat)) < 0)
            sb_line(&sb, "Query latency:    %s\n", UNAVAILABLE);
        else
            sb_line(&sb, "Query latency:    %s\n", lat);
    } else {
        sb_line(&sb, "Query latency:    %s\n", UNAVAILABLE);
    }

    sb_line(&sb, "Adaptation\n----------\n");
    if (metrics->has_adaptation) {
        sb_line(&sb, "Action:           %s\n",
                nexus_adapt_action_str(metrics->adaptation.action));
        sb_line(&sb, "Reason:           %s\n",
                nexus_adapt_reason_str(metrics->adaptation.reason));
        sb_line(&sb, "Target frames:    %lu\n",
                metrics->adaptation.target_frames);
        sb_line(&sb, "Pressure:         %.1f%%\n",
                metrics->adaptation.pressure * 100.0);
    } else {
        sb_line(&sb, "Action:           %s\n", UNAVAILABLE);
        sb_line(&sb, "Reason:           %s\n", UNAVAILABLE);
        sb_line(&sb, "Target frames:    %s\n", UNAVAILABLE);
        sb_line(&sb, "Pressure:         %s\n", UNAVAILABLE);
    }

    if (metrics->resize.available) {
        if (!metrics->resize.has_actual) {
            sb_line(&sb, "Resize applied:   %s\n", UNAVAILABLE);
        } else if (metrics->resize.applied) {
            sb_line(&sb, "Resize applied:   yes (%lu frames)\n",
                    metrics->resize.actual_frames);
        } else {
            sb_line(&sb, "Resize applied:   no (actual %lu frames)\n",
                    metrics->resize.actual_frames);
        }
    } else {
        sb_line(&sb, "Resize applied:   %s\n", UNAVAILABLE);
    }

    if (sb.overflow)
        return NEXUS_ERR_PROCESS; /* truncated: buffer too small */
    return NEXUS_OK;
}
