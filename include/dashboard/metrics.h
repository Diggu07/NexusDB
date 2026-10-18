/*
 * NexusDB — Live dashboard: metrics model (Rehan: Bridge & Observability).
 *
 * Collects and formats the numbers the dashboard displays. It is deliberately
 * separate from presentation so it can be unit-tested without a terminal,
 * ncurses, a database, or root.
 *
 * Honesty rules baked into the model:
 *   - Every group carries an explicit availability flag. Anything the database
 *     engine does not expose (cache hit/miss counters, query latency, a live
 *     pool) is reported as UNAVAILABLE, never as a fabricated zero.
 *   - Synthetic fixtures may be injected in tests, but production wiring must
 *     come from the real bridge/policy engine provider.
 *
 * Integration blockers (documented, not worked around):
 *   - BufferPool exposes capacity()/size()/pinned_frames() but no cache-hit or
 *     disk-read counters, and there is no query processor/latency API yet, so
 *     `has_cache_stats` and `has_latency` stay 0 until the engine adds them.
 */
#ifndef NEXUS_DASHBOARD_METRICS_H
#define NEXUS_DASHBOARD_METRICS_H

#include <stddef.h>

#include "bridge/adaptation_policy.h"
#include "bridge/resource_monitor.h"
#include "container/container.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Database-engine metrics. Each sub-group is optional and independently
 * flagged so a partial engine still renders honestly. */
typedef struct nexus_engine_metrics {
    int has_pool;                    /* pool target/actual/size available */
    unsigned long pool_target_frames;   /* requested capacity (from the policy) */
    unsigned long pool_actual_frames;   /* BufferPool::capacity() */
    unsigned long pool_resident_frames; /* BufferPool::size() */
    unsigned long pool_pinned_frames;   /* BufferPool::pinned_frames() */

    int has_cache_stats;             /* cache hit/miss counters available */
    unsigned long cache_hits;
    unsigned long cache_misses;

    int has_latency;                 /* query latency available */
    double latency_ms;
} nexus_engine_metrics_t;

/*
 * Provider the dashboard calls to fetch engine metrics. `context` is opaque.
 * Return NEXUS_OK and fill `out` (setting the has_* flags appropriately); any
 * non-OK return is treated as "engine metrics unavailable" (never fabricated).
 * Pass a NULL provider when there is no engine attached.
 */
typedef int (*nexus_engine_metrics_fn)(void *context, nexus_engine_metrics_t *out);

typedef struct nexus_dashboard_inputs {
    const nexus_resource_snapshot_t *resource; /* NULL or !valid => unavailable */
    int has_cpu_percent;                        /* CPU% needs two samples + time */
    double cpu_percent;
    const nexus_adapt_decision_t *adaptation;   /* NULL => unavailable */
} nexus_dashboard_inputs_t;

typedef struct nexus_dashboard_metrics {
    int has_resource;
    nexus_resource_snapshot_t resource;

    int has_cpu_percent;
    double cpu_percent;

    int has_adaptation;
    nexus_adapt_decision_t adaptation;

    nexus_engine_metrics_t engine; /* inner has_* flags carry availability */
} nexus_dashboard_metrics_t;

/* Zero a metrics struct: all optional groups unavailable. */
void nexus_dashboard_metrics_init(nexus_dashboard_metrics_t *metrics);

/*
 * Aggregate one dashboard frame. `inputs` and/or `provider` may be NULL.
 * Never fabricates: a NULL/invalid resource or a failing provider simply
 * leaves that group flagged unavailable. Returns NEXUS_ERR_INVALID on NULL out.
 */
int nexus_dashboard_collect(const nexus_dashboard_inputs_t *inputs,
                            nexus_engine_metrics_fn provider,
                            void *context,
                            nexus_dashboard_metrics_t *out);

/* ---- pure formatting helpers (independently testable) ---- */

/* Cache hit ratio hits/(hits+misses). Returns -1.0 when there were no
 * accesses (0/0), since the ratio is undefined rather than zero. */
double nexus_hit_ratio(unsigned long hits, unsigned long misses);

/* "512.0 MiB", "1024 B", ... Returns the snprintf length, or -1 for a NULL
 * buffer / zero length. */
int nexus_format_bytes(unsigned long bytes, char *buf, size_t len);

/* "75.0%" or "n/a" when there were no accesses. -1 on bad args. */
int nexus_format_hit_ratio(unsigned long hits, unsigned long misses,
                           char *buf, size_t len);

/* "12.3%". -1 on bad args. */
int nexus_format_percent(double percent, char *buf, size_t len);

/* "1.50 ms". -1 on bad args. */
int nexus_format_latency_ms(double ms, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_DASHBOARD_METRICS_H */
