/*
 * NexusDB — Benchmark harness implementation (Rehan).
 * See include/benchmark/harness.h for the contract and the honesty rules.
 */
#include "benchmark/harness.h"

#include <stdio.h>
#include <string.h>

#include "bridge/adaptation_policy.h"
#include "bridge/resource_monitor.h"
#include "container/container.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* Monotonic milliseconds. Windows uses QPC; POSIX uses CLOCK_MONOTONIC. */
static double bench_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    LARGE_INTEGER counter;

    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

/*
 * Join the current process to `cgroup_path` so the kernel's memory accounting
 * covers the workload running in this process. On non-Linux this is a no-op:
 * cgroups are not enforced, only a %TEMP% shim exists.
 */
static int bench_bind_self(const char *cgroup_path)
{
#ifdef _WIN32
    (void)cgroup_path;
    return NEXUS_OK;
#else
    char procs[NEXUS_PATH_MAX + 32];
    FILE *f;

    if (cgroup_path == NULL || cgroup_path[0] == '\0')
        return NEXUS_ERR_INVALID;
    snprintf(procs, sizeof(procs), "%s/cgroup.procs", cgroup_path);
    f = fopen(procs, "w");
    if (f == NULL)
        return NEXUS_ERR_PROCESS;
    if (fprintf(f, "%ld\n", (long)getpid()) < 0) {
        fclose(f);
        return NEXUS_ERR_PROCESS;
    }
    return fclose(f) == 0 ? NEXUS_OK : NEXUS_ERR_PROCESS;
#endif
}

/* Leave the container cgroup by moving back to its parent cgroup. On non-Linux
 * this is a no-op. Best effort: a failure only leaks the process into the
 * parent cgroup, which container_delete then refuses; the caller logs nothing. */
static int bench_unbind_self(const char *cgroup_path)
{
#ifdef _WIN32
    (void)cgroup_path;
    return NEXUS_OK;
#else
    char parent[NEXUS_PATH_MAX];
    char procs[NEXUS_PATH_MAX + 32];
    const char *slash;
    size_t n;
    FILE *f;

    if (cgroup_path == NULL || cgroup_path[0] == '\0')
        return NEXUS_ERR_INVALID;

    slash = strrchr(cgroup_path, '/');
    n = (slash != NULL) ? (size_t)(slash - cgroup_path) : strlen(cgroup_path);
    if (n >= sizeof(parent))
        n = sizeof(parent) - 1;
    memcpy(parent, cgroup_path, n);
    parent[n] = '\0';

    snprintf(procs, sizeof(procs), "%s/cgroup.procs", parent);
    f = fopen(procs, "w");
    if (f == NULL)
        return NEXUS_ERR_PROCESS;
    if (fprintf(f, "%ld\n", (long)getpid()) < 0) {
        fclose(f);
        return NEXUS_ERR_PROCESS;
    }
    return fclose(f) == 0 ? NEXUS_OK : NEXUS_ERR_PROCESS;
#endif
}

typedef struct bench_run_ctx {
    const nexus_bench_config_t *cfg;
    const nexus_bench_workload_t *wl;
    nexus_bench_result_t *res;
    nexus_policy_t policy;
    nexus_policy_config_t policy_cfg;
    nexus_resource_snapshot_t prev;
    int have_prev;
    double prev_ms;
    int have_pressure;
    int have_cpu;
    int have_latency;
    double cpu_sum;
    unsigned long cpu_count;
} bench_run_ctx_t;

static unsigned long bench_capacity(bench_run_ctx_t *rc)
{
    if (rc->wl->capacity == NULL)
        return 0;
    return rc->wl->capacity(rc->wl->ctx);
}

static int bench_resize_request(void *ctx, unsigned long target_frames)
{
    bench_run_ctx_t *rc = (bench_run_ctx_t *)ctx;
    unsigned long cap;
    int status;

    if (rc->wl->resize == NULL)
        return NEXUS_ERR_PROCESS;

    status = rc->wl->resize(rc->wl->ctx, target_frames);
    cap = bench_capacity(rc);
    if (cap > rc->res->pool.max_frames)
        rc->res->pool.max_frames = cap;
    if (cap < rc->res->pool.min_frames)
        rc->res->pool.min_frames = cap;
    return status;
}

static void bench_sample(bench_run_ctx_t *rc,
                         nexus_bench_sampler_fn sampler,
                         void *sampler_ctx)
{
    nexus_resource_snapshot_t snap;
    nexus_adapt_decision_t dec;
    double t = bench_now_ms();
    int have = 0;
    unsigned long cur;

    memset(&snap, 0, sizeof(snap));
    if (sampler != NULL) {
        if (sampler(sampler_ctx, &snap) == NEXUS_OK)
            have = 1;
    } else if (nexus_bridge_read_container(rc->cfg->container_name, &snap) == NEXUS_OK) {
        have = 1;
    }

    rc->res->resource.samples++;

    if (have && snap.valid) {
        if (!rc->have_pressure) {
            rc->res->resource.pressure_min = snap.memory_pressure;
            rc->have_pressure = 1;
        } else if (snap.memory_pressure < rc->res->resource.pressure_min) {
            rc->res->resource.pressure_min = snap.memory_pressure;
        }
        if (snap.memory_pressure > rc->res->resource.pressure_max)
            rc->res->resource.pressure_max = snap.memory_pressure;
        rc->res->resource.pressure_last = snap.memory_pressure;

        if (snap.has_limit) {
            rc->res->resource.saw_limit = 1;
            rc->res->resource.memory_max_bytes = snap.memory_max;
        }
        rc->res->resource.memory_current_last = snap.memory_current;

        if (rc->have_prev && snap.has_cpu) {
            double pct = nexus_bridge_cpu_percent(&rc->prev, &snap,
                                                  (long long)(t - rc->prev_ms));
            if (pct >= 0.0) {
                if (!rc->have_cpu) {
                    rc->res->resource.cpu_percent_min = pct;
                    rc->res->resource.cpu_percent_max = pct;
                    rc->have_cpu = 1;
                } else {
                    if (pct < rc->res->resource.cpu_percent_min)
                        rc->res->resource.cpu_percent_min = pct;
                    if (pct > rc->res->resource.cpu_percent_max)
                        rc->res->resource.cpu_percent_max = pct;
                }
                rc->res->resource.have_cpu = 1;
                rc->cpu_sum += pct;
                rc->cpu_count++;
            }
        }

        /* Only a valid snapshot becomes the baseline for the next CPU rate. */
        rc->prev = snap;
        rc->prev_ms = t;
        rc->have_prev = 1;
    } else {
        /* No reading: reset the baseline so a later sample is not paired with
         * a stale one and cannot produce a bogus rate. */
        rc->have_prev = 0;
    }

    cur = bench_capacity(rc);
    if (nexus_policy_apply(&rc->policy, &rc->policy_cfg, &snap, cur,
                           bench_resize_request, rc, &dec) != NEXUS_OK) {
        rc->res->pool.failed_resizes++;
    }

    if (dec.action == NEXUS_ADAPT_GROW)
        rc->res->pool.grow_requests++;
    else if (dec.action == NEXUS_ADAPT_SHRINK)
        rc->res->pool.shrink_requests++;
    else
        rc->res->pool.hold_decisions++;
}

void nexus_bench_config_default(nexus_bench_config_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->scenario = "default";
    cfg->container_name = "nexusbench";
    cfg->db_path = "data/benchmark.db";
    cfg->engine_path = "";
    cfg->limit_mb = 512;
    cfg->page_count = 4096;
    cfg->iterations = 4096;
    cfg->sample_every = 32;
    cfg->pool_frames_start = 256;
    cfg->dirty_every = 8;
    cfg->random = 0;
    cfg->random_seed = 1;
    cfg->manage_container = 1;
}

static void copy_label(char *dst, size_t len, const char *src)
{
    size_t n;
    if (src == NULL)
        src = "unnamed";
    n = strlen(src);
    if (n >= len)
        n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int nexus_bench_run(const nexus_bench_config_t *cfg,
                    const nexus_bench_workload_t *wl,
                    nexus_bench_sampler_fn sampler,
                    void *sampler_ctx,
                    nexus_bench_result_t *out)
{
    bench_run_ctx_t rc;
    char cgroup_path[NEXUS_PATH_MAX];
    int have_cgroup = 0;
    int bound = 0;
    unsigned long every;
    unsigned long i;
    int status;

    if (cfg == NULL || wl == NULL || out == NULL)
        return NEXUS_ERR_INVALID;
    if (wl->open == NULL || wl->step == NULL || wl->close == NULL)
        return NEXUS_ERR_INVALID;
    if (!cfg->manage_container && sampler == NULL)
        return NEXUS_ERR_INVALID;

    memset(out, 0, sizeof(*out));
    memset(&rc, 0, sizeof(rc));
    cgroup_path[0] = '\0';

    copy_label(out->scenario, sizeof(out->scenario), cfg->scenario);
    out->limit_mb = cfg->limit_mb;
    out->page_count = cfg->page_count;
    out->iterations = cfg->iterations;
    out->sample_every = cfg->sample_every;
    out->pool_frames_start = cfg->pool_frames_start;
    out->dirty_every = cfg->dirty_every;
    out->random = cfg->random ? 1 : 0;
    out->random_seed = cfg->random_seed;
#ifdef _WIN32
    out->platform_linux = 0;
#else
    out->platform_linux = 1;
#endif
    out->container_managed = cfg->manage_container ? 1 : 0;
    out->hit_ratio = -1.0;
    out->engine_reports_cache_counters = 0;
    out->engine_reports_query_latency = 0;

    rc.cfg = cfg;
    rc.wl = wl;
    rc.res = out;
    nexus_policy_init(&rc.policy);
    nexus_policy_config_default(&rc.policy_cfg);

    if (cfg->manage_container) {
        const nexus_container_t *c;

        /* limit_mb is at most a few hundred, so the byte product fits 32-bit. */
        status = container_create(cfg->container_name,
                                  cfg->limit_mb * 1024UL * 1024UL,
                                  50, cfg->engine_path);
        if (status == NEXUS_ERR_EXISTS) {
            (void)container_stop(cfg->container_name);
            (void)container_delete(cfg->container_name);
            status = container_create(cfg->container_name,
                                      cfg->limit_mb * 1024UL * 1024UL,
                                      50, cfg->engine_path);
        }
        if (status != NEXUS_OK)
            return status;

        c = container_get(cfg->container_name);
        if (c == NULL)
            return NEXUS_ERR_STATE;
        copy_label(cgroup_path, sizeof(cgroup_path), c->cgroup_path);
        have_cgroup = (cgroup_path[0] != '\0');

        if (have_cgroup && bench_bind_self(cgroup_path) == NEXUS_OK)
            bound = 1;
        out->self_bound_to_cgroup = bound;

        status = container_start(cfg->container_name);
        if (status != NEXUS_OK) {
            if (bound)
                (void)bench_unbind_self(cgroup_path);
            (void)container_stop(cfg->container_name);
            (void)container_delete(cfg->container_name);
            return status;
        }
    }

    status = wl->open(wl->ctx, cfg);
    if (status != NEXUS_OK) {
        if (cfg->manage_container) {
            if (bound)
                (void)bench_unbind_self(cgroup_path);
            (void)container_stop(cfg->container_name);
            (void)container_delete(cfg->container_name);
        }
        return status;
    }

    out->pool.start_frames = bench_capacity(&rc);
    out->pool.min_frames = out->pool.start_frames;
    out->pool.max_frames = out->pool.start_frames;

    every = (cfg->sample_every == 0) ? 1 : cfg->sample_every;
    for (i = 0; i < cfg->iterations; i++) {
        double t0;
        double t1;
        double lat;
        int hit = 0;
        int s;

        if ((i % every) == 0)
            bench_sample(&rc, sampler, sampler_ctx);

        t0 = bench_now_ms();
        s = wl->step(wl->ctx, i, &hit);
        t1 = bench_now_ms();
        lat = t1 - t0;

        if (s != NEXUS_OK) {
            out->fetch_errors++;
            continue;
        }
        out->steps_run++;
        if (hit)
            out->cache_hits++;
        else
            out->cache_misses++;

        if (!rc.have_latency) {
            out->fetch_ms_min = lat;
            out->fetch_ms_max = lat;
            rc.have_latency = 1;
        } else {
            if (lat < out->fetch_ms_min)
                out->fetch_ms_min = lat;
            if (lat > out->fetch_ms_max)
                out->fetch_ms_max = lat;
        }
        out->fetch_ms_total += lat;
    }

    bench_sample(&rc, sampler, sampler_ctx);

    out->pool.end_frames = bench_capacity(&rc);
    if (out->pool.end_frames > out->pool.max_frames)
        out->pool.max_frames = out->pool.end_frames;
    if (out->pool.end_frames < out->pool.min_frames)
        out->pool.min_frames = out->pool.end_frames;
    if (wl->resident != NULL)
        out->pool.resident_last = wl->resident(wl->ctx);
    if (wl->pinned != NULL)
        out->pool.pinned_last = wl->pinned(wl->ctx);

    if (out->steps_run > 0)
        out->fetch_ms_avg = out->fetch_ms_total / (double)out->steps_run;
    if ((out->cache_hits + out->cache_misses) > 0) {
        out->hit_ratio = (double)out->cache_hits /
                         (double)(out->cache_hits + out->cache_misses);
    }
    if (rc.cpu_count > 0)
        out->resource.cpu_percent_avg = rc.cpu_sum / (double)rc.cpu_count;

    wl->close(wl->ctx);

    if (cfg->manage_container) {
        if (bound)
            (void)bench_unbind_self(cgroup_path);
        (void)container_stop(cfg->container_name);
        (void)container_delete(cfg->container_name);
    }

    return NEXUS_OK;
}

int nexus_bench_environment(char *buf, size_t len)
{
    int n;

    if (buf == NULL || len == 0)
        return NEXUS_ERR_INVALID;
#ifdef _WIN32
    n = snprintf(buf, len,
                 "platform: Windows (dev shim; cgroups not enforced)\n"
                 "pointer_width_bits: %u\n",
                 (unsigned)(sizeof(void *) * 8));
#else
    n = snprintf(buf, len,
                 "platform: Linux (cgroups v2)\n"
                 "pointer_width_bits: %u\n"
                 "page_size_bytes: 4096\n",
                 (unsigned)(sizeof(void *) * 8));
#endif
    if (n < 0)
        return NEXUS_ERR_PROCESS;
    return NEXUS_OK;
}

int nexus_bench_result_write(const nexus_bench_result_t *out,
                             const char *path,
                             const char *env_block)
{
    FILE *f;
    int to_stdout = (path == NULL);

    if (out == NULL)
        return NEXUS_ERR_INVALID;

    if (to_stdout) {
        f = stdout;
    } else {
        f = fopen(path, "w");
        if (f == NULL)
            return NEXUS_ERR_PROCESS;
    }

    fprintf(f, "NexusDB Benchmark Result\n");
    fprintf(f, "========================\n");
    fprintf(f, "status: RUN (completed)\n");
    fprintf(f, "scenario: %s\n", out->scenario);
    fprintf(f, "limit_mb: %lu\n", out->limit_mb);
    fprintf(f, "page_count: %lu\n", out->page_count);
    fprintf(f, "iterations: %lu\n", out->iterations);
    fprintf(f, "sample_every: %lu\n", out->sample_every);
    fprintf(f, "pool_frames_start: %lu\n", out->pool_frames_start);
    fprintf(f, "dirty_every: %lu\n", out->dirty_every);
    fprintf(f, "order: %s\n", out->random ? "random" : "sequential");
    fprintf(f, "random_seed: %lu\n", out->random_seed);

    fprintf(f, "\nenvironment\n-----------\n");
    fprintf(f, "platform_linux: %d\n", out->platform_linux);
    fprintf(f, "container_managed: %d\n", out->container_managed);
    fprintf(f, "self_bound_to_cgroup: %d\n", out->self_bound_to_cgroup);

    fprintf(f, "\nworkload (measured)\n-------------------\n");
    fprintf(f, "steps_run: %lu\n", out->steps_run);
    fprintf(f, "cache_hits: %lu\n", out->cache_hits);
    fprintf(f, "cache_misses: %lu\n", out->cache_misses);
    if (out->hit_ratio < 0.0)
        fprintf(f, "cache_hit_ratio: n/a\n");
    else
        fprintf(f, "cache_hit_ratio: %.4f\n", out->hit_ratio);
    fprintf(f, "fetch_errors: %lu\n", out->fetch_errors);
    fprintf(f, "operation_latency_ms: min=%.4f max=%.4f avg=%.4f total=%.4f\n",
            out->fetch_ms_min, out->fetch_ms_max, out->fetch_ms_avg,
            out->fetch_ms_total);

    fprintf(f, "\nbuffer_pool (measured)\n----------------------\n");
    fprintf(f, "start_frames: %lu\n", out->pool.start_frames);
    fprintf(f, "end_frames: %lu\n", out->pool.end_frames);
    fprintf(f, "min_frames: %lu\n", out->pool.min_frames);
    fprintf(f, "max_frames: %lu\n", out->pool.max_frames);
    fprintf(f, "resident_frames_last: %lu\n", out->pool.resident_last);
    fprintf(f, "pinned_frames_last: %lu\n", out->pool.pinned_last);
    fprintf(f, "grow_requests: %lu\n", out->pool.grow_requests);
    fprintf(f, "shrink_requests: %lu\n", out->pool.shrink_requests);
    fprintf(f, "hold_decisions: %lu\n", out->pool.hold_decisions);
    fprintf(f, "failed_resizes: %lu\n", out->pool.failed_resizes);

    fprintf(f, "\nresources (measured)\n--------------------\n");
    fprintf(f, "samples: %lu\n", out->resource.samples);
    fprintf(f, "memory_limit_bytes: %lu (saw_limit=%d)\n",
            out->resource.memory_max_bytes, out->resource.saw_limit);
    fprintf(f, "memory_current_last: %lu\n", out->resource.memory_current_last);
    fprintf(f, "pressure: min=%.4f max=%.4f last=%.4f\n",
            out->resource.pressure_min, out->resource.pressure_max,
            out->resource.pressure_last);
    fprintf(f, "cpu_percent: min=%.2f max=%.2f avg=%.2f (have_cpu=%d)\n",
            out->resource.cpu_percent_min, out->resource.cpu_percent_max,
            out->resource.cpu_percent_avg, out->resource.have_cpu);

    fprintf(f, "\nunavailable_metrics (NOT measured; never fabricated)\n"
               "---------------------------------------------------\n");
    fprintf(f, "engine_reported_cache_counters: unavailable "
               "(BufferPool exposes none; hit/miss observed by the harness via Contains())\n");
    fprintf(f, "engine_reported_query_latency: unavailable "
               "(no query processor on this branch; per-step Fetch time reported above)\n");

    if (env_block != NULL) {
        fprintf(f, "\nenvironment_capture\n-------------------\n");
        fprintf(f, "%s\n", env_block);
    }

    if (!to_stdout) {
        if (fclose(f) != 0)
            return NEXUS_ERR_PROCESS;
    } else {
        fflush(f);
    }
    return NEXUS_OK;
}

const char *nexus_bench_strerror(int rc)
{
    switch (rc) {
    case NEXUS_OK:            return "ok";
    case NEXUS_ERR_INVALID:   return "invalid argument";
    case NEXUS_ERR_NOT_FOUND: return "container not found";
    case NEXUS_ERR_EXISTS:    return "container already exists";
    case NEXUS_ERR_STATE:     return "illegal state for this operation";
    case NEXUS_ERR_NOMEM:     return "container registry full";
    case NEXUS_ERR_PROCESS:   return "operation failed";
    default:                  return "unknown error";
    }
}
