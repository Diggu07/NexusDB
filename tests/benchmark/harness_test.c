/*
 * NexusDB — Benchmark harness SELF-TEST (Rehan).
 *
 * Exercises the harness ORCHESTRATION (sampling loop, policy wiring, pool
 * resize accounting, hit/miss + latency accumulation, result formatting) with a
 * SYNTHETIC workload provider and a SCRIPTED resource sampler.
 *
 * This is NOT a benchmark and produces NO benchmark numbers. Its synthetic
 * inputs exist only to drive deterministic control-flow checks; the real
 * benchmark (benchmarks/nexus_benchmark.c) refuses to run without the actual
 * database engine. Runs on the Windows dev machine (no cgroup enforcement, no
 * ncurses, no DB engine). Exit 0 on success, 1 on failure.
 */
/* POSIX declarations (nanosleep) must be exposed before any system header is
 * included: glibc hides them under -std=c11 (strict ANSI) unless a
 * feature-test macro is defined first. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "benchmark/harness.h"
#include "container/container.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

static int CloseTo(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d < 1e-9;
}

static void tiny_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---- synthetic workload provider (test-only) ---- */
typedef struct fake_wl {
    unsigned long capacity;
} fake_wl_t;

static int fake_open(void *ctx, const nexus_bench_config_t *cfg)
{
    ((fake_wl_t *)ctx)->capacity = cfg->pool_frames_start;
    return NEXUS_OK;
}

static int fake_step(void *ctx, unsigned long index, int *cache_hit)
{
    (void)ctx;
    tiny_sleep_ms(2); /* ensures a measurable CPU/latency interval */
    *cache_hit = (index % 2) == 0 ? 1 : 0;
    return NEXUS_OK;
}

static unsigned long fake_capacity(void *ctx)
{
    return ((fake_wl_t *)ctx)->capacity;
}

static unsigned long fake_resident(void *ctx)
{
    (void)ctx;
    return 42; /* sentinel: proves the harness records resident frames */
}

static unsigned long fake_pinned(void *ctx)
{
    (void)ctx;
    return 3; /* sentinel: proves the harness records pinned frames */
}

static int fake_resize(void *ctx, unsigned long target_frames)
{
    if (target_frames == 0)
        return NEXUS_ERR_INVALID;
    ((fake_wl_t *)ctx)->capacity = target_frames;
    return NEXUS_OK;
}

static void fake_close(void *ctx)
{
    (void)ctx;
}

/* ---- scripted sampler (test-only) ---- */
typedef struct fake_sampler {
    unsigned long calls;
} fake_sampler_t;

static const double kPressures[] = {0.30, 0.90, 0.50, 0.93, 0.40, 0.95};

static int fake_sample(void *ctx, nexus_resource_snapshot_t *out)
{
    fake_sampler_t *s = (fake_sampler_t *)ctx;
    double p = kPressures[s->calls % 6];

    memset(out, 0, sizeof(*out));
    out->valid = 1;
    out->has_memory = 1;
    out->has_limit = 1;
    out->has_cpu = 1;
    out->memory_max = 512UL * 1024UL * 1024UL;
    out->memory_current = (unsigned long)(p * (double)out->memory_max);
    out->memory_pressure = p;
    out->cpu_usage_usec = 1000UL * s->calls; /* strictly increasing */
    s->calls++;
    return NEXUS_OK;
}

static void temp_path(char *buf, size_t len, const char *name)
{
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (base == NULL)
        base = ".";
    snprintf(buf, len, "%s\\%s", base, name);
#else
    snprintf(buf, len, "/tmp/%s", name);
#endif
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    char buf[8192];
    size_t n;
    int found;

    if (f == NULL)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    found = strstr(buf, needle) != NULL;
    return found;
}

int main(void)
{
    nexus_bench_config_t cfg;
    nexus_bench_workload_t wl;
    nexus_bench_result_t res;
    fake_wl_t state;
    fake_sampler_t sampler;
    int rc;

    printf("=== NexusDB Benchmark Harness SELF-TEST (synthetic; NOT a benchmark) ===\n\n");

    /* ---- defaults ---- */
    nexus_bench_config_default(&cfg);
    CHECK("default limit 512 MiB", cfg.limit_mb == 512);
    CHECK("default iterations 4096", cfg.iterations == 4096);
    CHECK("default sample_every 32", cfg.sample_every == 32);
    CHECK("default pool 256 frames", cfg.pool_frames_start == 256);
    CHECK("default manage_container on", cfg.manage_container == 1);

    /* ---- argument validation ---- */
    CHECK("run NULL cfg -> INVALID",
          nexus_bench_run(NULL, NULL, NULL, NULL, &res) == NEXUS_ERR_INVALID);
    CHECK("run NULL workload -> INVALID",
          nexus_bench_run(&cfg, NULL, NULL, NULL, &res) == NEXUS_ERR_INVALID);

    /* manage_container=0 requires an external sampler */
    cfg.manage_container = 0;
    CHECK("run no-container + no sampler -> INVALID",
          nexus_bench_run(&cfg, &wl, NULL, NULL, &res) == NEXUS_ERR_INVALID);

    /* ---- happy path with synthetic provider + scripted sampler ---- */
    memset(&state, 0, sizeof(state));
    memset(&sampler, 0, sizeof(sampler));
    memset(&wl, 0, sizeof(wl));
    wl.ctx = &state;
    wl.open = fake_open;
    wl.step = fake_step;
    wl.capacity = fake_capacity;
    wl.resident = fake_resident;
    wl.pinned = fake_pinned;
    wl.resize = fake_resize;
    wl.close = fake_close;

    cfg.scenario = "selftest";
    cfg.limit_mb = 512;
    cfg.iterations = 6;
    cfg.sample_every = 1;
    cfg.pool_frames_start = 256;
    cfg.manage_container = 0;

    rc = nexus_bench_run(&cfg, &wl, fake_sample, &sampler, &res);
    CHECK("run synthetic -> OK", rc == NEXUS_OK);
    CHECK("scenario echoed", strcmp(res.scenario, "selftest") == 0);
    CHECK("platform flag set", res.platform_linux == 0 || res.platform_linux == 1);
    CHECK("container not managed", res.container_managed == 0);

    /* ---- workload accounting ---- */
    CHECK("steps_run == iterations", res.steps_run == 6);
    CHECK("hits+misses == steps", res.cache_hits + res.cache_misses == 6);
    CHECK("hits == 3 (even indices)", res.cache_hits == 3);
    CHECK("hit ratio 0.5", CloseTo(res.hit_ratio, 0.5));
    CHECK("no fetch errors", res.fetch_errors == 0);
    CHECK("latency min <= avg", res.fetch_ms_min <= res.fetch_ms_avg);
    CHECK("latency avg <= max", res.fetch_ms_avg <= res.fetch_ms_max);
    CHECK("latency avg > 0", res.fetch_ms_avg > 0.0);

    /* ---- policy drove real resize accounting ---- */
    CHECK("pool start 256", res.pool.start_frames == 256);
    CHECK("pool grew at least once", res.pool.grow_requests >= 1);
    CHECK("pool shrank at least once", res.pool.shrink_requests >= 1);
    CHECK("no failed resizes", res.pool.failed_resizes == 0);
    CHECK("pool max 512", res.pool.max_frames == 512);
    CHECK("pool min 256", res.pool.min_frames == 256);
    CHECK("pool end 512", res.pool.end_frames == 512);
    CHECK("resident frames recorded", res.pool.resident_last == 42);
    CHECK("pinned frames recorded", res.pool.pinned_last == 3);

    /* ---- resource samples ---- */
    CHECK("samples taken", res.resource.samples >= 6);
    CHECK("saw a finite limit", res.resource.saw_limit == 1);
    CHECK("limit echoed", res.resource.memory_max_bytes == 512UL * 1024UL * 1024UL);
    CHECK("pressure max >= 0.93", res.resource.pressure_max >= 0.93);
    CHECK("pressure min <= 0.30", res.resource.pressure_min <= 0.30);
    CHECK("cpu sample computed", res.resource.have_cpu == 1);
    CHECK("cpu min >= 0", res.resource.cpu_percent_min >= 0.0);

    /* ---- honesty flags ---- */
    CHECK("engine cache counters unavailable",
          res.engine_reports_cache_counters == 0);
    CHECK("engine query latency unavailable",
          res.engine_reports_query_latency == 0);

    /* ---- environment helper ---- */
    {
        char env[256];
        CHECK("environment fills buffer",
              nexus_bench_environment(env, sizeof(env)) == NEXUS_OK && env[0] != '\0');
        CHECK("environment invalid args",
              nexus_bench_environment(NULL, 0) == NEXUS_ERR_INVALID);
    }

    /* ---- result file writing ---- */
    {
        char path[512];
        char env[256];
        temp_path(path, sizeof(path), "nexus_bench_selftest_result.txt");
        (void)nexus_bench_environment(env, sizeof(env));

        CHECK("result_write -> OK",
              nexus_bench_result_write(&res, path, env) == NEXUS_OK);
        CHECK("file has RUN status", file_contains(path, "status: RUN (completed)"));
        CHECK("file has scenario", file_contains(path, "scenario: selftest"));
        CHECK("file marks query latency unavailable",
              file_contains(path, "engine_reported_query_latency: unavailable"));
        CHECK("file marks cache counters unavailable",
              file_contains(path, "engine_reported_cache_counters: unavailable"));
        CHECK("file echoes environment_capture",
              file_contains(path, "environment_capture"));
        remove(path);

        CHECK("result_write NULL out -> INVALID",
              nexus_bench_result_write(NULL, path, NULL) == NEXUS_ERR_INVALID);
        CHECK("result_write bad path -> PROCESS",
              nexus_bench_result_write(&res, "no/such/dir/result.txt", NULL) == NEXUS_ERR_PROCESS);
    }

    /* ---- sample_every == 0 must be treated as 1 (no divide-by-zero) ---- */
    {
        nexus_bench_result_t r2;
        fake_sampler_t s2;
        memset(&s2, 0, sizeof(s2));
        memset(&state, 0, sizeof(state));
        cfg.iterations = 4;
        cfg.sample_every = 0;
        cfg.pool_frames_start = 256;
        rc = nexus_bench_run(&cfg, &wl, fake_sample, &s2, &r2);
        CHECK("sample_every 0 runs safely", rc == NEXUS_OK && r2.steps_run == 4);
    }

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
