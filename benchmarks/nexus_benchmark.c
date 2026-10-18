/*
 * NexusDB — Benchmark runner entry point (Rehan: Bridge & Observability).
 *
 * Parses a scenario, runs the harness against the real database engine, and
 * writes a self-describing result file. When the DB engine is not compiled in
 * (e.g. the Windows dev machine), it refuses to run: benchmark numbers must
 * come from the real engine, never from synthetic substitutes.
 *
 * Build:
 *   Linux (real):    make benchmark        -> build/nexus_benchmark
 *   portable stub:   make                   -> build/nexus_benchmark_stub
 *
 * Usage:
 *   nexus_benchmark --limit-mb 512 --iterations 4096 --out results/x.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "benchmark/harness.h"
#include "container/container.h"

static void usage(const char *argv0)
{
    printf("Usage: %s [options]\n", argv0);
    printf("  --limit-mb N        memory limit in MiB (default 512)\n");
    printf("  --iterations N      workload steps (default 4096)\n");
    printf("  --pages N           logical pages walked (default 4096)\n");
    printf("  --pool N            initial buffer-pool frames (default 256)\n");
    printf("  --sample-every N    cgroup sample interval in steps (default 32)\n");
    printf("  --dirty-every N     mark every Nth fetched page dirty, 0=read-only (default 8)\n");
    printf("  --random            random page order (default sequential)\n");
    printf("  --seed N            random seed (default 1)\n");
    printf("  --scenario S        scenario label (default \"default\")\n");
    printf("  --container S       container name (default \"nexusbench\")\n");
    printf("  --db PATH           database file (default data/benchmark.db)\n");
    printf("  --engine PATH       recorded db_engine path (default \"\")\n");
    printf("  --no-container      do not manage a container (needs external sampler)\n");
    printf("  --out PATH          write the result here (default: stdout)\n");
    printf("  --help              show this help\n");
}

int main(int argc, char **argv)
{
    nexus_bench_config_t cfg;
    const char *out_path = NULL;
    int i;

    nexus_bench_config_default(&cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--limit-mb") == 0 && i + 1 < argc) {
            cfg.limit_mb = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            cfg.iterations = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc) {
            cfg.page_count = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--pool") == 0 && i + 1 < argc) {
            cfg.pool_frames_start = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--sample-every") == 0 && i + 1 < argc) {
            cfg.sample_every = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--dirty-every") == 0 && i + 1 < argc) {
            cfg.dirty_every = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--random") == 0) {
            cfg.random = 1;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cfg.random_seed = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            cfg.scenario = argv[++i];
        } else if (strcmp(argv[i], "--container") == 0 && i + 1 < argc) {
            cfg.container_name = argv[++i];
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            cfg.db_path = argv[++i];
        } else if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
            cfg.engine_path = argv[++i];
        } else if (strcmp(argv[i], "--no-container") == 0) {
            cfg.manage_container = 0;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    printf("=== NexusDB Benchmark ===\n");

#ifndef NEXUS_HAVE_DB_ENGINE
    (void)cfg;
    (void)out_path;
    fprintf(stderr,
            "Status: NOT RUN.\n"
            "The database-engine workload is not built into this binary, so no\n"
            "real measurements can be produced. Build on Linux with the DB engine\n"
            "(GCC 11+, C++17):\n"
            "    make benchmark\n"
            "Refusing to substitute synthetic or estimated numbers.\n");
    return 3;
#else
    {
        nexus_bench_workload_t wl;
        nexus_bench_result_t res;
        char env[256];
        int rc;

        memset(&wl, 0, sizeof(wl));
        rc = nexus_bench_engine_workload(&wl);
        if (rc != NEXUS_OK) {
            fprintf(stderr, "engine workload unavailable: %s\n",
                    nexus_bench_strerror(rc));
            return 3;
        }

        if (nexus_bench_environment(env, sizeof(env)) != NEXUS_OK)
            env[0] = '\0';

        rc = nexus_bench_run(&cfg, &wl, NULL, NULL, &res);
        if (rc != NEXUS_OK) {
            fprintf(stderr, "benchmark failed: %s\n", nexus_bench_strerror(rc));
            return 1;
        }

        rc = nexus_bench_result_write(&res, out_path, env);
        if (rc != NEXUS_OK) {
            fprintf(stderr, "failed to write result%s%s\n",
                    out_path ? " to " : "", out_path ? out_path : "");
            return 1;
        }

        if (out_path != NULL)
            printf("result written to %s\n", out_path);
        printf("steps=%lu hits=%lu misses=%lu pool %lu->%lu frames\n",
               res.steps_run, res.cache_hits, res.cache_misses,
               res.pool.start_frames, res.pool.end_frames);
        return 0;
    }
#endif
}
