/*
 * NexusDB — Live dashboard: metrics aggregation + formatting implementation.
 * See include/dashboard/metrics.h for the contract.
 */
#include "dashboard/metrics.h"

#include <stdio.h>
#include <string.h>

void nexus_dashboard_metrics_init(nexus_dashboard_metrics_t *metrics)
{
    if (metrics == NULL)
        return;
    memset(metrics, 0, sizeof(*metrics));
}

int nexus_dashboard_collect(const nexus_dashboard_inputs_t *inputs,
                            nexus_engine_metrics_fn provider,
                            void *context,
                            nexus_dashboard_metrics_t *out)
{
    if (out == NULL)
        return NEXUS_ERR_INVALID;

    nexus_dashboard_metrics_init(out);

    if (inputs != NULL) {
        if (inputs->resource != NULL && inputs->resource->valid) {
            out->resource = *inputs->resource;
            out->has_resource = 1;
        }
        out->has_cpu_percent = inputs->has_cpu_percent ? 1 : 0;
        out->cpu_percent = inputs->cpu_percent;
        if (inputs->adaptation != NULL) {
            out->adaptation = *inputs->adaptation;
            out->has_adaptation = 1;
        }
    }

    if (provider != NULL) {
        nexus_engine_metrics_t em;
        memset(&em, 0, sizeof(em));
        if (provider(context, &em) == NEXUS_OK) {
            out->engine = em; /* availability flags travel inside `em` */
        }
        /* provider failure => engine stays unavailable; nothing fabricated */
    }

    return NEXUS_OK;
}

double nexus_hit_ratio(unsigned long hits, unsigned long misses)
{
    double total = (double)hits + (double)misses;
    if (total <= 0.0)
        return -1.0;
    return (double)hits / total;
}

int nexus_format_bytes(unsigned long bytes, char *buf, size_t len)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value;
    int unit = 0;

    if (buf == NULL || len == 0)
        return -1;

    value = (double)bytes;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0)
        return snprintf(buf, len, "%lu B", bytes);
    return snprintf(buf, len, "%.1f %s", value, units[unit]);
}

int nexus_format_hit_ratio(unsigned long hits, unsigned long misses,
                           char *buf, size_t len)
{
    double ratio;

    if (buf == NULL || len == 0)
        return -1;

    ratio = nexus_hit_ratio(hits, misses);
    if (ratio < 0.0)
        return snprintf(buf, len, "n/a");
    return snprintf(buf, len, "%.1f%%", ratio * 100.0);
}

int nexus_format_percent(double percent, char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return -1;
    return snprintf(buf, len, "%.1f%%", percent);
}

int nexus_format_latency_ms(double ms, char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return -1;
    return snprintf(buf, len, "%.2f ms", ms);
}
