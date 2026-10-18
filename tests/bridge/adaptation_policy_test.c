/*
 * NexusDB — OS–DB Bridge adaptation-policy test (Rehan).
 *
 * Pure, root-free unit tests for the memory-pressure policy. Snapshots are
 * built directly (the file-reading path is covered by
 * tests/bridge/resource_monitor_test.c). A fake resize sink records requests
 * and can be made to fail, so the integration boundary is exercised without a
 * database. Exit 0 on success, 1 on failure.
 */
#include <stdio.h>
#include <string.h>

#include "bridge/adaptation_policy.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

/* 1 MiB/... limit in bytes: page_size 4096 * 1000 frames = 4,096,000 bytes. */
#define LIMIT_BYTES 4096000UL

/* Tolerance compare: 32-bit x87 keeps float literals at extended precision,
 * so exact `==` on a stored double is not portable (matches the repo's other
 * tests, which use a 1e-9 epsilon). */
static int CloseTo(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d < 1e-9;
}

static nexus_resource_snapshot_t Snap(double pressure, unsigned long limit_bytes)
{
    nexus_resource_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.valid = 1;
    s.has_memory = 1;
    s.has_limit = (limit_bytes > 0);
    s.memory_max = limit_bytes;
    s.memory_pressure = pressure;
    s.memory_current = (unsigned long)(pressure * (double)limit_bytes);
    return s;
}

static void CfgCustom(nexus_policy_config_t *c)
{
    nexus_policy_config_default(c);
    c->shrink_threshold = 0.85;
    c->grow_threshold = 0.60;
    c->pool_fraction = 0.6;   /* ceiling = 1000 * 0.6 = 600 frames */
    c->page_size = 4096;
    c->min_frames = 10;
    c->max_frames = 0;
    c->max_step_down = 256;
    c->max_step_up = 256;
}

/* ---- fake resize sink ---- */
static int g_sink_calls = 0;
static unsigned long g_sink_target = 0;
static int g_sink_rc = NEXUS_OK;

static int FakeSink(void *context, unsigned long target)
{
    (void)context;
    g_sink_calls++;
    g_sink_target = target;
    return g_sink_rc;
}

static void ResetSink(void)
{
    g_sink_calls = 0;
    g_sink_target = 0;
    g_sink_rc = NEXUS_OK;
}

int main(void)
{
    nexus_policy_config_t cfg;
    nexus_policy_t policy;
    nexus_adapt_decision_t d;
    nexus_resource_snapshot_t s;

    printf("=== NexusDB OS-DB Bridge: adaptation policy test ===\n\n");

    /* ---- defaults and validation ---- */
    nexus_policy_config_default(&cfg);
    CHECK("default shrink threshold 0.85", CloseTo(cfg.shrink_threshold, 0.85));
    CHECK("default grow threshold 0.60", CloseTo(cfg.grow_threshold, 0.60));
    CHECK("default page size 4096", cfg.page_size == 4096);
    CHECK("default config valid", nexus_policy_config_valid(&cfg) == 1);
    {
        nexus_policy_config_t bad = cfg;
        bad.grow_threshold = 0.90; /* >= shrink */
        CHECK("invalid config: grow >= shrink rejected",
              nexus_policy_config_valid(&bad) == 0);
        bad = cfg;
        bad.pool_fraction = 0.0;
        CHECK("invalid config: pool_fraction 0 rejected",
              nexus_policy_config_valid(&bad) == 0);
        bad = cfg;
        bad.pool_fraction = 1.5;
        CHECK("invalid config: pool_fraction > 1 rejected",
              nexus_policy_config_valid(&bad) == 0);
        bad = cfg;
        bad.page_size = 0;
        CHECK("invalid config: page_size 0 rejected",
              nexus_policy_config_valid(&bad) == 0);
        bad = cfg;
        bad.min_frames = 0;
        CHECK("invalid config: min_frames 0 rejected",
              nexus_policy_config_valid(&bad) == 0);
        bad = cfg;
        bad.min_frames = 100;
        bad.max_frames = 50;
        CHECK("invalid config: max < min rejected",
              nexus_policy_config_valid(&bad) == 0);
        CHECK("invalid config: NULL rejected",
              nexus_policy_config_valid(NULL) == 0);
    }

    nexus_policy_init(&policy);
    CHECK("evaluate NULL out -> INVALID",
          nexus_policy_evaluate(&policy, &cfg, NULL, 100, NULL) == NEXUS_ERR_INVALID);

    /* ---- unavailable / unlimited inputs never fabricate action ---- */
    {
        nexus_policy_config_t good;
        CfgCustom(&good);
        nexus_policy_init(&policy);
        s = Snap(0.99, LIMIT_BYTES);
        s.valid = 0;
        CHECK("invalid snapshot -> HOLD",
              nexus_policy_evaluate(&policy, &good, &s, 100, &d) == NEXUS_OK &&
                  d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_NO_METRICS);

        s = Snap(0.99, LIMIT_BYTES);
        s.has_memory = 0;
        CHECK("no memory reading -> HOLD (no metrics)",
              nexus_policy_evaluate(&policy, &good, &s, 100, &d) == NEXUS_OK &&
                  d.reason == NEXUS_REASON_NO_METRICS);

        s = Snap(0.0, 0); /* unlimited */
        CHECK("unlimited memory -> HOLD",
              nexus_policy_evaluate(&policy, &good, &s, 100, &d) == NEXUS_OK &&
                  d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_UNLIMITED);
    }

    /* ---- low pressure grows (bounded by step and ceiling) ---- */
    CfgCustom(&cfg);
    nexus_policy_init(&policy);
    s = Snap(0.30, LIMIT_BYTES);
    {
        int rc = nexus_policy_evaluate(&policy, &cfg, &s, 100, &d);
        printf("--- low pressure 0.30, current=100 -> %s target=%lu (%s) ---\n",
               nexus_adapt_action_str(d.action), d.target_frames,
               nexus_adapt_reason_str(d.reason));
        CHECK("low pressure -> GROW", rc == NEXUS_OK && d.action == NEXUS_ADAPT_GROW);
        CHECK("low pressure target = current + step (356)", d.target_frames == 356);
    }

    /* ---- high pressure shrinks ---- */
    nexus_policy_init(&policy);
    s = Snap(0.95, LIMIT_BYTES);
    {
        int rc = nexus_policy_evaluate(&policy, &cfg, &s, 1000, &d);
        printf("--- high pressure 0.95, current=1000 -> %s target=%lu (%s) ---\n",
               nexus_adapt_action_str(d.action), d.target_frames,
               nexus_adapt_reason_str(d.reason));
        CHECK("high pressure -> SHRINK", rc == NEXUS_OK && d.action == NEXUS_ADAPT_SHRINK);
        CHECK("high pressure target = current - step (744)", d.target_frames == 744);
    }

    /* ---- hysteresis dead-band ---- */
    nexus_policy_init(&policy);
    s = Snap(0.70, LIMIT_BYTES);
    CHECK("mid pressure -> HOLD within band",
          nexus_policy_evaluate(&policy, &cfg, &s, 300, &d) == NEXUS_OK &&
              d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_WITHIN_BAND);

    nexus_policy_init(&policy);
    s = Snap(0.85, LIMIT_BYTES); /* exactly at shrink threshold */
    CHECK("pressure == shrink threshold -> SHRINK",
          nexus_policy_evaluate(&policy, &cfg, &s, 1000, &d) == NEXUS_OK &&
              d.action == NEXUS_ADAPT_SHRINK);

    nexus_policy_init(&policy);
    s = Snap(0.849, LIMIT_BYTES); /* just below */
    CHECK("pressure just below shrink threshold -> HOLD",
          nexus_policy_evaluate(&policy, &cfg, &s, 1000, &d) == NEXUS_OK &&
              d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_WITHIN_BAND);

    nexus_policy_init(&policy);
    s = Snap(0.60, LIMIT_BYTES); /* exactly at grow threshold */
    CHECK("pressure == grow threshold -> GROW",
          nexus_policy_evaluate(&policy, &cfg, &s, 100, &d) == NEXUS_OK &&
              d.action == NEXUS_ADAPT_GROW);

    nexus_policy_init(&policy);
    s = Snap(0.601, LIMIT_BYTES); /* just above */
    CHECK("pressure just above grow threshold -> HOLD",
          nexus_policy_evaluate(&policy, &cfg, &s, 100, &d) == NEXUS_OK &&
              d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_WITHIN_BAND);

    /* ---- floor and ceiling bounds ---- */
    nexus_policy_init(&policy);
    s = Snap(0.95, LIMIT_BYTES);
    CHECK("shrink at min_frames -> HOLD (AT_MIN)",
          nexus_policy_evaluate(&policy, &cfg, &s, 10, &d) == NEXUS_OK &&
              d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_AT_MIN);

    nexus_policy_init(&policy);
    s = Snap(0.30, LIMIT_BYTES);
    CHECK("grow at ceiling (600) -> HOLD (AT_MAX)",
          nexus_policy_evaluate(&policy, &cfg, &s, 600, &d) == NEXUS_OK &&
              d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_AT_MAX);

    nexus_policy_init(&policy);
    s = Snap(0.30, LIMIT_BYTES);
    {
        int rc = nexus_policy_evaluate(&policy, &cfg, &s, 500, &d);
        CHECK("grow step clamps to ceiling (600)", rc == NEXUS_OK &&
              d.action == NEXUS_ADAPT_GROW && d.target_frames == 600);
    }

    /* explicit max_frames ceiling tighter than the limit-derived one */
    {
        nexus_policy_config_t capped;
        CfgCustom(&capped);
        capped.max_frames = 200;
        nexus_policy_init(&policy);
        s = Snap(0.30, LIMIT_BYTES);
        CHECK("max_frames caps growth (current 150 -> 200)",
              nexus_policy_evaluate(&policy, &capped, &s, 150, &d) == NEXUS_OK &&
                  d.target_frames == 200);
    }

    /* ---- duplicate suppression: identical reading is not re-requested ---- */
    {
        nexus_policy_config_t good;
        CfgCustom(&good);
        nexus_policy_init(&policy);
        s = Snap(0.95, LIMIT_BYTES);
        CHECK("first high-pressure read -> SHRINK 744",
              nexus_policy_evaluate(&policy, &good, &s, 1000, &d) == NEXUS_OK &&
                  d.action == NEXUS_ADAPT_SHRINK && d.target_frames == 744);
        CHECK("identical second read -> HOLD (duplicate)",
              nexus_policy_evaluate(&policy, &good, &s, 1000, &d) == NEXUS_OK &&
                  d.action == NEXUS_ADAPT_HOLD && d.reason == NEXUS_REASON_DUPLICATE);
        /* after the shrink is applied, current moves and stepping resumes. */
        CHECK("after apply (current 744), next read -> SHRINK 488",
              nexus_policy_evaluate(&policy, &good, &s, 744, &d) == NEXUS_OK &&
                  d.action == NEXUS_ADAPT_SHRINK && d.target_frames == 488);
    }

    /* ---- integration boundary: apply invokes the sink only on GROW/SHRINK ---- */
    {
        nexus_policy_config_t good;
        CfgCustom(&good);
        nexus_policy_init(&policy);

        ResetSink();
        s = Snap(0.30, LIMIT_BYTES);
        {
            int rc = nexus_policy_apply(&policy, &good, &s, 100, FakeSink, NULL, &d);
            CHECK("apply GROW -> sink called once", rc == NEXUS_OK && g_sink_calls == 1);
            CHECK("apply passes the target frames (356)", g_sink_target == 356);
        }

        ResetSink();
        s = Snap(0.70, LIMIT_BYTES);
        {
            int rc = nexus_policy_apply(&policy, &good, &s, 300, FakeSink, NULL, &d);
            CHECK("apply HOLD -> sink not called", rc == NEXUS_OK && g_sink_calls == 0);
        }

        /* a failing sink surfaces its code and can be retried */
        ResetSink();
        g_sink_rc = -7;
        nexus_policy_init(&policy);
        s = Snap(0.95, LIMIT_BYTES);
        {
            int rc = nexus_policy_apply(&policy, &good, &s, 1000, FakeSink, NULL, &d);
            CHECK("apply failure returns sink code", rc == -7);
            CHECK("apply failure marks REQUEST_FAILED",
                  d.reason == NEXUS_REASON_REQUEST_FAILED);
            CHECK("apply failure attempted the resize", g_sink_calls == 1);
        }
        ResetSink();
        {
            int rc = nexus_policy_apply(&policy, &good, &s, 1000, FakeSink, NULL, &d);
            CHECK("retry after failure re-issues the request",
                  rc == NEXUS_OK && g_sink_calls == 1 && g_sink_target == 744);
        }

        /* NULL sink with a real decision is a programming error */
        nexus_policy_init(&policy);
        s = Snap(0.95, LIMIT_BYTES);
        CHECK("apply with NULL sink and GROW/SHRINK -> INVALID",
              nexus_policy_apply(&policy, &good, &s, 1000, NULL, NULL, &d) ==
                  NEXUS_ERR_INVALID);
    }

    /* ---- diagnostic strings ---- */
    CHECK("action string never NULL", nexus_adapt_action_str(NEXUS_ADAPT_HOLD) != NULL);
    CHECK("reason string never NULL", nexus_adapt_reason_str(NEXUS_REASON_OK) != NULL);

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
