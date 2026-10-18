/*
 * NexusDB — Benchmark harness (Rehan: Bridge & Observability).
 *
 * Drives the real OS–DB Bridge path end to end under a memory limit:
 *
 *   container limit (cgroups v2)
 *     -> cgroup resource snapshot (bridge)
 *       -> memory-pressure adaptation policy
 *         -> buffer-pool resize request (engine public API)
 *           -> dashboard/benchmark metrics
 *
 * What this harness MEASURES (only real, observable quantities):
 *   - Buffer-pool resizing: capacity before/after each policy request, and the
 *     count of GROW/SHRINK/HOLD decisions (real BufferPool::Resize calls).
 *   - Memory pressure and memory.current/limit: read from the cgroup snapshot
 *     through the bridge; never a fabricated value.
 *   - CPU usage: from two cpu.stat samples (nexus_bridge_cpu_percent).
 *   - Cache hit/miss ratio: the harness probes BufferPool::Contains() BEFORE
 *     each Fetch and counts a hit when the page was already resident. This is
 *     harness-observed cache behaviour, NOT an engine-reported counter.
 *   - Operation latency: wall-clock around each workload step (Fetch path).
 *
 * What this harness does NOT measure (and will not fake):
 *   - Engine-reported cache counters: BufferPool exposes no hit/miss counters,
 *     so `engine_reports_cache_counters` is always 0.
 *   - Query latency: there is no query processor on this branch, so
 *     `engine_reports_query_latency` is always 0. The measured per-step time is
 *     labelled "operation latency" and is a different quantity.
 *
 * Reproducibility: every knob is explicit in nexus_bench_config_t and echoed
 * into the result so a run is self-describing. The harness never invents data:
 * if the engine workload provider is NULL, or the engine was not built, the
 * benchmark refuses to run instead of substituting synthetic numbers.
 */
#ifndef NEXUS_BENCHMARK_HARNESS_H
#define NEXUS_BENCHMARK_HARNESS_H

#include <stddef.h>

#include "bridge/adaptation_policy.h"
#include "bridge/resource_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One reproducible benchmark scenario. */
typedef struct nexus_bench_config {
    const char *scenario;         /* short label, e.g. "512MiB-sequential" */
    const char *container_name;   /* container to create/measure (see manage_container) */
    const char *db_path;          /* database file the workload opens */
    const char *engine_path;      /* recorded db_engine path (may be NULL/"") */
    unsigned long limit_mb;       /* memory limit in MiB (512 / 256 / 128) */
    unsigned long page_count;     /* logical pages the workload walks */
    unsigned long iterations;     /* number of workload steps */
    unsigned long sample_every;   /* take a cgroup sample every N steps (>= 1) */
    unsigned long pool_frames_start; /* initial buffer-pool capacity in frames */
    unsigned long dirty_every;    /* mark every Nth fetched page dirty (0 = read-only) */
    int random;                   /* 1 = random page order, 0 = sequential */
    unsigned long random_seed;    /* seed for the random walk */
    int manage_container;         /* 1 = create/start/stop/delete the container */
} nexus_bench_config_t;

/*
 * The workload provider backed by the real database engine (BufferPool over
 * PageStorage). Implemented in engine_workload.cpp (C++17, Linux/GCC 11+).
 * The harness never fabricates engine behaviour: a NULL provider is an error.
 *
 *   open   : open storage + create the pool at pool_frames_start
 *   step    : perform one unit of work; set *cache_hit = 1 when the target
 *             page was already resident BEFORE the Fetch (Contains() probe)
 *   capacity/resident/pinned : current pool counters (engine public API)
 *   resize  : request a new capacity (engine public API), NEXUS_OK on success
 *   close   : flush and release engine resources
 */
typedef struct nexus_bench_workload {
    void *ctx;
    int (*open)(void *ctx, const nexus_bench_config_t *cfg);
    int (*step)(void *ctx, unsigned long index, int *cache_hit);
    unsigned long (*capacity)(void *ctx);
    unsigned long (*resident)(void *ctx);
    unsigned long (*pinned)(void *ctx);
    int (*resize)(void *ctx, unsigned long target_frames);
    void (*close)(void *ctx);
} nexus_bench_workload_t;

/*
 * Resource sampler. When NULL, the harness reads the container's cgroup via the
 * bridge (nexus_bridge_read_container). A non-NULL sampler lets a self-test
 * inject a scripted sequence; it must still fill a REAL-looking snapshot with
 * honest availability flags (valid/has_memory/has_limit/has_cpu).
 */
typedef int (*nexus_bench_sampler_fn)(void *ctx, nexus_resource_snapshot_t *out);

typedef struct nexus_bench_pool_stats {
    unsigned long start_frames;
    unsigned long end_frames;
    unsigned long min_frames;
    unsigned long max_frames;
    unsigned long resident_last;    /* BufferPool::size() at the end */
    unsigned long pinned_last;      /* BufferPool::pinned_frames() at the end */
    unsigned long grow_requests;    /* GROW decisions issued */
    unsigned long shrink_requests;  /* SHRINK decisions issued */
    unsigned long hold_decisions;   /* HOLD decisions (incl. no-metrics/unlimited) */
    unsigned long failed_resizes;   /* resize requests that returned an error */
} nexus_bench_pool_stats_t;

typedef struct nexus_bench_resource_stats {
    unsigned long samples;
    int saw_limit;                  /* any sample reported a finite limit */
    unsigned long memory_max_bytes; /* last finite limit seen (0 if none) */
    unsigned long memory_current_last;
    double pressure_min;
    double pressure_max;
    double pressure_last;
    int have_cpu;                   /* at least one CPU% sample computed */
    double cpu_percent_min;
    double cpu_percent_max;
    double cpu_percent_avg;
} nexus_bench_resource_stats_t;

typedef struct nexus_bench_result {
    /* self-describing config echo */
    char scenario[64];
    unsigned long limit_mb;
    unsigned long page_count;
    unsigned long iterations;
    unsigned long sample_every;
    unsigned long pool_frames_start;
    unsigned long dirty_every;
    int random;
    unsigned long random_seed;

    /* environment */
    int platform_linux;             /* 1 = Linux; 0 = non-Linux (dev shim) */
    int container_managed;
    int self_bound_to_cgroup;       /* 1 when the process joined the container cgroup */

    /* measured: workload */
    unsigned long steps_run;
    unsigned long cache_hits;       /* harness-observed via Contains() */
    unsigned long cache_misses;
    double hit_ratio;               /* -1 when there were 0 accesses */
    unsigned long fetch_errors;
    double fetch_ms_min;
    double fetch_ms_max;
    double fetch_ms_avg;
    double fetch_ms_total;

    nexus_bench_pool_stats_t pool;
    nexus_bench_resource_stats_t resource;

    /* honesty: capabilities the engine does NOT expose on this branch */
    int engine_reports_cache_counters;  /* always 0 */
    int engine_reports_query_latency;   /* always 0 */
} nexus_bench_result_t;

/* Fill `cfg` with documented defaults (512 MiB, sequential, 4096 iterations,
 * sample every 32 steps, pool 256 frames, page_count 4096, manage container). */
void nexus_bench_config_default(nexus_bench_config_t *cfg);

/*
 * Run one scenario. `wl` must be a real engine workload provider (NULL is an
 * error). `sampler` may be NULL (use the bridge on cfg->container_name).
 *
 * When cfg->manage_container is 1 the harness creates/starts the container
 * (real cgroups v2 on Linux, %TEMP% shim on Windows), joins its own process to
 * the container cgroup so the readings cover the running workload, samples,
 * adapts, resizes, then leaves the cgroup and deletes the container. When 0 the
 * caller owns all container setup and must supply a sampler.
 *
 * Returns NEXUS_OK when the run completed (out is filled), or a NEXUS_* error.
 */
int nexus_bench_run(const nexus_bench_config_t *cfg,
                    const nexus_bench_workload_t *wl,
                    nexus_bench_sampler_fn sampler,
                    void *sampler_ctx,
                    nexus_bench_result_t *out);

/*
 * Write `out` as a self-describing text report. `env_block` is printed verbatim
 * (may be NULL); pass the machine/environment capture here. `path` NULL writes
 * to stdout. Returns NEXUS_OK or NEXUS_ERR_PROCESS on an I/O failure.
 */
int nexus_bench_result_write(const nexus_bench_result_t *out,
                             const char *path,
                             const char *env_block);

/*
 * Path to the built-in environment block is caller-supplied. This helper fills
 * a small compile/runtime environment string into `buf` (platform, pointer
 * width, page size assumption). Returns NEXUS_OK; never writes past `len`.
 */
int nexus_bench_environment(char *buf, size_t len);

/* Human-readable status code (never NULL). */
const char *nexus_bench_strerror(int rc);

/*
 * Fill `out` with the real database-engine workload provider (BufferPool over
 * PageStorage). Implemented in engine_workload.cpp; only linked when the
 * benchmark is built with the DB engine (Linux, GCC 11+). Returns NEXUS_OK or a
 * NEXUS_* error. Declared unconditionally so callers can guard on
 * NEXUS_HAVE_DB_ENGINE.
 */
int nexus_bench_engine_workload(nexus_bench_workload_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_BENCHMARK_HARNESS_H */
