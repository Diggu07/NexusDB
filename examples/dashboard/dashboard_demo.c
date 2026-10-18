/*
 * NexusDB — Live dashboard demo (Rehan).
 *
 * Wires the real pipeline end to end using ONLY public APIs:
 *   container runtime -> OS-DB Bridge snapshot -> adaptation policy
 *   -> dashboard metrics -> plain-text (or ncurses) render.
 *
 * Honesty: no database engine is attached on this branch, so the engine
 * metrics provider is NULL and cache/latency/pool fields render as
 * "unavailable". The sample interval and iteration count keep the plain mode
 * fast for regression runs.
 *
 * Usage: nexus_dashboard_demo [name] [memory_mb] [iterations] [--ncurses]
 *   iterations 0 = run until Ctrl-C (plain) / until 'q' (ncurses)
 */
/* POSIX declarations (clock_gettime/CLOCK_MONOTONIC, nanosleep) must be exposed
 * before any system header is included: glibc hides them under -std=c11
 * (strict ANSI) unless a feature-test macro is defined first. */
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

#include "bridge/adaptation_policy.h"
#include "bridge/resource_monitor.h"
#include "container/container.h"
#include "dashboard/dashboard.h"
#include "dashboard/metrics.h"

/* Nominal pool size used only to feed the policy when no engine is attached;
 * clearly not a real pool (engine metrics stay unavailable). */
#define NOMINAL_FRAMES 2048UL
#define SAMPLE_INTERVAL_MS 500

static long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int ms)
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

struct demo_ctx {
    const char *name;
    nexus_policy_config_t cfg;
    nexus_policy_t policy;
    nexus_resource_snapshot_t prev;
    int have_prev;
    long long last_ms;
};

static int build_frame(void *context, nexus_dashboard_metrics_t *out)
{
    struct demo_ctx *d = context;
    nexus_resource_snapshot_t snap;
    nexus_adapt_decision_t dec;
    nexus_dashboard_inputs_t in;
    int have_cpu = 0;
    double cpu_pct = 0.0;
    long long t = now_ms();

    memset(&snap, 0, sizeof(snap));
    /* Real resource read through the container runtime's cgroup directory. */
    (void)nexus_bridge_read_container(d->name, &snap);

    if (d->have_prev && d->prev.has_cpu && snap.has_cpu) {
        double pct = nexus_bridge_cpu_percent(&d->prev, &snap, t - d->last_ms);
        if (pct >= 0.0) {
            have_cpu = 1;
            cpu_pct = pct;
        }
    }
    d->prev = snap;
    d->have_prev = 1;
    d->last_ms = t;

    memset(&dec, 0, sizeof(dec));
    (void)nexus_policy_evaluate(&d->policy, &d->cfg, &snap, NOMINAL_FRAMES, &dec);

    memset(&in, 0, sizeof(in));
    in.resource = &snap;
    in.has_cpu_percent = have_cpu;
    in.cpu_percent = cpu_pct;
    in.adaptation = &dec;

    /* NULL provider: no engine attached -> engine metrics unavailable. */
    return nexus_dashboard_collect(&in, NULL, NULL, out);
}

int main(int argc, char **argv)
{
    const char *name = (argc > 1) ? argv[1] : "nexusdash";
    unsigned long memory_mb = (argc > 2) ? strtoul(argv[2], NULL, 10) : 512UL;
    long iterations = (argc > 3) ? strtol(argv[3], NULL, 10) : 3;
    int use_ncurses = 0;
    struct demo_ctx d;
    long i;
    int rc;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--ncurses") == 0)
            use_ncurses = 1;

    printf("=== NexusDB Live Dashboard (demo) ===\n");
    printf("platform: %s\n",
#ifdef _WIN32
           "Windows dev shim (mock cgroup files, no kernel enforcement)"
#else
           "Linux cgroups v2"
#endif
    );

    if (memory_mb == 0)
        memory_mb = 512UL;

    rc = container_create(name, memory_mb * 1024UL * 1024UL, 50, NULL);
    if (rc != NEXUS_OK) {
        fprintf(stderr, "container_create('%s') failed: %s\n", name,
                container_strerror(rc));
        return 2;
    }
    (void)container_start(name);

    memset(&d, 0, sizeof(d));
    d.name = name;
    nexus_policy_config_default(&d.cfg);
    nexus_policy_init(&d.policy);

    if (use_ncurses) {
#ifdef NEXUS_HAVE_NCURSES
        rc = nexus_dashboard_run_ncurses(build_frame, &d, SAMPLE_INTERVAL_MS,
                                         iterations <= 0 ? 0u
                                                         : (unsigned)iterations);
        if (rc != NEXUS_OK)
            fprintf(stderr, "ncurses dashboard returned %d\n", rc);
#else
        printf("built without ncurses; falling back to plain mode\n");
        use_ncurses = 0;
#endif
    }

    if (!use_ncurses) {
        char text[2048];
        for (i = 0; iterations <= 0 || i < iterations; i++) {
            nexus_dashboard_metrics_t m;
            if (build_frame(&d, &m) != NEXUS_OK) {
                fprintf(stderr, "frame collection failed\n");
                break;
            }
            if (nexus_dashboard_render_text(&m, text, sizeof(text)) == NEXUS_OK)
                printf("\n%s", text);
            if (iterations <= 0 || i + 1 < iterations)
                sleep_ms(SAMPLE_INTERVAL_MS);
        }
    }

    (void)container_stop(name);
    (void)container_delete(name);
    printf("\n[demo complete]\n");
    return 0;
}
