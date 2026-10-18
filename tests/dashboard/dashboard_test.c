/*
 * NexusDB — Live dashboard test (Rehan).
 *
 * Pure, terminal-free tests for metric aggregation, formatting, the cache-hit
 * ratio, unavailable/zero/invalid handling, and the plain-text renderer.
 * Synthetic fixtures are used ONLY here (labelled); production wiring must use
 * the real bridge/policy provider. Exit 0 on success, 1 on failure.
 */
#include <stdio.h>
#include <string.h>

#include "dashboard/dashboard.h"
#include "dashboard/metrics.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

static int Has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static int CloseTo(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d < 1e-9;
}

static nexus_resource_snapshot_t Snap(double pressure, unsigned long limit)
{
    nexus_resource_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.valid = 1;
    s.has_memory = 1;
    s.has_limit = (limit > 0);
    s.memory_max = limit;
    s.memory_pressure = pressure;
    s.memory_current = (unsigned long)(pressure * (double)limit);
    return s;
}

/* ---- synthetic providers (test-only) ---- */
static int FullProvider(void *ctx, nexus_engine_metrics_t *out)
{
    (void)ctx;
    out->has_pool = 1;
    out->pool_target_frames = 600;
    out->pool_actual_frames = 600;
    out->pool_resident_frames = 412;
    out->pool_pinned_frames = 3;
    out->has_cache_stats = 1;
    out->cache_hits = 75;
    out->cache_misses = 25;
    out->has_latency = 1;
    out->latency_ms = 1.5;
    return NEXUS_OK;
}

static int NoAccessProvider(void *ctx, nexus_engine_metrics_t *out)
{
    (void)ctx;
    out->has_cache_stats = 1;
    out->cache_hits = 0;
    out->cache_misses = 0;
    return NEXUS_OK;
}

static int FailProvider(void *ctx, nexus_engine_metrics_t *out)
{
    (void)ctx;
    (void)out;
    return NEXUS_ERR_PROCESS;
}

int main(void)
{
    char b[64];
    char text[2048];
    nexus_dashboard_metrics_t m;
    nexus_dashboard_inputs_t in;

    printf("=== NexusDB Live Dashboard: metrics + render test ===\n\n");

    /* ---- formatting ---- */
    CHECK("bytes 0", nexus_format_bytes(0, b, sizeof(b)) > 0 && strcmp(b, "0 B") == 0);
    CHECK("bytes 512", strcmp((nexus_format_bytes(512, b, sizeof(b)), b), "512 B") == 0);
    CHECK("bytes 1024 -> 1.0 KiB",
          strcmp((nexus_format_bytes(1024, b, sizeof(b)), b), "1.0 KiB") == 0);
    CHECK("bytes 1048576 -> 1.0 MiB",
          strcmp((nexus_format_bytes(1048576, b, sizeof(b)), b), "1.0 MiB") == 0);
    CHECK("bytes 536870912 -> 512.0 MiB",
          strcmp((nexus_format_bytes(536870912UL, b, sizeof(b)), b), "512.0 MiB") == 0);
    CHECK("bytes invalid NULL", nexus_format_bytes(1, NULL, 8) == -1);
    CHECK("bytes invalid len 0", nexus_format_bytes(1, b, 0) == -1);

    CHECK("percent", strcmp((nexus_format_percent(12.34, b, sizeof(b)), b), "12.3%") == 0);
    CHECK("percent invalid", nexus_format_percent(1.0, NULL, 8) == -1);
    CHECK("latency", strcmp((nexus_format_latency_ms(1.5, b, sizeof(b)), b), "1.50 ms") == 0);
    CHECK("latency invalid", nexus_format_latency_ms(1.0, b, 0) == -1);

    /* ---- hit ratio (denominator = hits + misses) ---- */
    CHECK("hit ratio 75/25 = 0.75", CloseTo(nexus_hit_ratio(75, 25), 0.75));
    CHECK("hit ratio 10/0 = 1.0", CloseTo(nexus_hit_ratio(10, 0), 1.0));
    CHECK("hit ratio 0/10 = 0.0", CloseTo(nexus_hit_ratio(0, 10), 0.0));
    CHECK("hit ratio 0/0 undefined (-1)", nexus_hit_ratio(0, 0) < 0.0);
    CHECK("format ratio 75/25", strcmp((nexus_format_hit_ratio(75, 25, b, sizeof(b)), b),
                                       "75.0%") == 0);
    CHECK("format ratio 0/0 = n/a",
          strcmp((nexus_format_hit_ratio(0, 0, b, sizeof(b)), b), "n/a") == 0);
    CHECK("format ratio invalid", nexus_format_hit_ratio(1, 1, NULL, 8) == -1);

    /* ---- aggregation availability ---- */
    nexus_dashboard_metrics_init(&m);
    CHECK("init: engine pool unavailable", m.engine.has_pool == 0);
    CHECK("init: cache stats unavailable", m.engine.has_cache_stats == 0);
    CHECK("collect NULL out -> INVALID",
          nexus_dashboard_collect(NULL, NULL, NULL, NULL) == NEXUS_ERR_INVALID);

    {
        nexus_dashboard_metrics_t out;
        nexus_dashboard_inputs_t empty;
        memset(&empty, 0, sizeof(empty));
        CHECK("collect empty -> OK", nexus_dashboard_collect(&empty, NULL, NULL, &out) == NEXUS_OK);
        CHECK("collect empty: no resource", out.has_resource == 0);
        CHECK("collect empty: no adaptation", out.has_adaptation == 0);
        CHECK("collect empty: no engine pool", out.engine.has_pool == 0);

        /* provider failure is treated as unavailable, not fabricated */
        CHECK("collect failing provider -> OK",
              nexus_dashboard_collect(&empty, FailProvider, NULL, &out) == NEXUS_OK);
        CHECK("collect failing provider: engine unavailable", out.engine.has_pool == 0);
    }

    /* invalid resource snapshot is not shown as a valid reading */
    {
        nexus_resource_snapshot_t bad = Snap(0.5, 1000);
        nexus_dashboard_inputs_t in2;
        nexus_dashboard_metrics_t out;
        bad.valid = 0;
        memset(&in2, 0, sizeof(in2));
        in2.resource = &bad;
        CHECK("collect invalid resource -> OK",
              nexus_dashboard_collect(&in2, NULL, NULL, &out) == NEXUS_OK);
        CHECK("collect invalid resource: has_resource 0", out.has_resource == 0);
    }

    /* ---- render: everything unavailable ---- */
    {
        nexus_dashboard_metrics_t out;
        nexus_dashboard_inputs_t empty;
        memset(&empty, 0, sizeof(empty));
        (void)nexus_dashboard_collect(&empty, NULL, NULL, &out);
        CHECK("render text OK",
              nexus_dashboard_render_text(&out, text, sizeof(text)) == NEXUS_OK);
        CHECK("render shows unavailable memory", Has(text, "Memory used:      unavailable"));
        CHECK("render shows unavailable latency", Has(text, "Query latency:    unavailable"));
        CHECK("render shows unavailable adaptation", Has(text, "Action:           unavailable"));
        CHECK("render NULL metrics -> INVALID",
              nexus_dashboard_render_text(NULL, text, sizeof(text)) == NEXUS_ERR_INVALID);
        CHECK("render NULL buffer -> INVALID",
              nexus_dashboard_render_text(&out, NULL, 16) == NEXUS_ERR_INVALID);
        CHECK("render tiny buffer -> PROCESS (truncation)",
              nexus_dashboard_render_text(&out, text, 16) == NEXUS_ERR_PROCESS);
    }

    /* ---- render: unlimited + full engine + adaptation ---- */
    {
        nexus_resource_snapshot_t res = Snap(0.30, 0); /* unlimited */
        nexus_adapt_decision_t dec;
        nexus_dashboard_metrics_t out;
        memset(&dec, 0, sizeof(dec));
        dec.action = NEXUS_ADAPT_GROW;
        dec.reason = NEXUS_REASON_OK;
        dec.target_frames = 356;
        dec.pressure = 0.30;

        memset(&in, 0, sizeof(in));
        in.resource = &res;
        in.has_cpu_percent = 1;
        in.cpu_percent = 12.3;
        in.adaptation = &dec;

        CHECK("collect full -> OK",
              nexus_dashboard_collect(&in, FullProvider, NULL, &out) == NEXUS_OK);
        CHECK("collect full: engine pool available", out.engine.has_pool == 1);
        CHECK("collect full: cache stats available", out.engine.has_cache_stats == 1);
        CHECK("collect full: latency available", out.engine.has_latency == 1);

        CHECK("render full OK",
              nexus_dashboard_render_text(&out, text, sizeof(text)) == NEXUS_OK);
        printf("--- rendered frame ---\n%s----------------------\n", text);
        CHECK("render shows unlimited limit", Has(text, "Memory limit:     unlimited"));
        CHECK("render shows memory used", Has(text, "Memory used:      0 B"));
        CHECK("render shows cpu", Has(text, "CPU:              12.3%"));
        CHECK("render shows buffer target", Has(text, "Buffer target:    600 frames"));
        CHECK("render shows cache hit ratio", Has(text, "Cache hit ratio:  75.0%"));
        CHECK("render shows latency", Has(text, "Query latency:    1.50 ms"));
        CHECK("render shows adaptation action", Has(text, "Action:           GROW"));
        CHECK("render shows adaptation target", Has(text, "Target frames:    356"));
    }

    /* ---- render: cache available but zero accesses -> n/a, not 0% ---- */
    {
        nexus_dashboard_metrics_t out;
        nexus_dashboard_inputs_t empty;
        memset(&empty, 0, sizeof(empty));
        (void)nexus_dashboard_collect(&empty, NoAccessProvider, NULL, &out);
        CHECK("render zero-access OK",
              nexus_dashboard_render_text(&out, text, sizeof(text)) == NEXUS_OK);
        CHECK("render zero-access hit ratio n/a", Has(text, "Cache hit ratio:  n/a"));
        CHECK("render zero-access hits 0", Has(text, "Cache hits:       0"));
    }

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
