/*
 * NexusDB — cgroup v2 CPU enforcement test (P4).
 *
 * Proves that cpu.max is actually enforced by the kernel, not merely written:
 * a controlled, wall-clock-bounded CPU workload (nexus_cpu_workload) is run
 * twice inside disposable cgroups — once unrestricted and once throttled to
 * 20% of one CPU — and the kernel's cpu.stat is compared.
 *
 * Enforcement evidence (read directly from the cgroup the test created):
 *   - unrestricted run: usage_usec > 0 and nr_throttled == 0
 *   - throttled run:    nr_throttled >= 1 and throttled_usec > 0, with
 *                       usage_usec materially below the unrestricted run
 *
 * The workload is attached to its cgroup while still blocked before exec (via
 * process_spawn_gated), so it can never run outside the limit, even briefly.
 *
 * Requires a real, writable cgroup v2 hierarchy (a delegated systemd user
 * session with NEXUS_CGROUP_ROOT set). Where that is unavailable — a private
 * root, an unprivileged shell, or the Windows shim — the test reports
 * ENV-LIMITED and exits 0 rather than claiming a result it did not observe.
 *
 * Exit 0 on success or ENV-LIMITED, 1 on a real assertion failure.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define getpid_compat() _getpid()
#else
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define getpid_compat() (int)getpid()
#endif

#include "container/cgroup.h"
#include "container/container.h"
#include "container/process.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

#ifndef _WIN32

typedef struct {
    unsigned long long usage_usec;
    unsigned long long nr_periods;
    unsigned long long nr_throttled;
    unsigned long long throttled_usec;
} cpustat_t;

static void path_join(char *buf, size_t len, const char *dir, const char *file)
{
    snprintf(buf, len, "%s/%s", dir, file);
}

/* Read cpu.stat. Returns 0 on success, -1 if the file is missing/unreadable
 * (which for our purposes means "not a real kernel cgroup"). */
static int read_cpu_stat(const char *cg, cpustat_t *out)
{
    char p[NEXUS_PATH_MAX + 32];
    FILE *f;
    char line[256];

    memset(out, 0, sizeof(*out));
    path_join(p, sizeof(p), cg, "cpu.stat");
    f = fopen(p, "r");
    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long v;
        if (sscanf(line, "usage_usec %llu", &v) == 1)
            out->usage_usec = v;
        else if (sscanf(line, "nr_periods %llu", &v) == 1)
            out->nr_periods = v;
        else if (sscanf(line, "nr_throttled %llu", &v) == 1)
            out->nr_throttled = v;
        else if (sscanf(line, "throttled_usec %llu", &v) == 1)
            out->throttled_usec = v;
    }
    fclose(f);
    return 0;
}

static int file_exists(const char *dir, const char *file)
{
    char p[NEXUS_PATH_MAX + 32];
    FILE *f;
    path_join(p, sizeof(p), dir, file);
    f = fopen(p, "r");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

/* A real cgroup2 directory exposes kernel-owned files; a plain directory (or
 * the Windows shim) does not. Used to turn "cannot demonstrate enforcement"
 * into an honest ENV-LIMITED, never a false PASS. */
static int is_real_cgroup(const char *cg)
{
    return file_exists(cg, "cgroup.controllers") &&
           file_exists(cg, "cpu.stat") &&
           file_exists(cg, "memory.max");
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* Reap pid within timeout_ms. Returns 0 if reaped (fills *st), -1 on timeout,
 * -2 if the child was already reaped. */
static int wait_bounded(long pid, int timeout_ms, process_status_t *st)
{
    int waited = 0;
    while (waited <= timeout_ms) {
        int status;
        pid_t w = waitpid((pid_t)pid, &status, WNOHANG);
        if (w == (pid_t)pid) {
            st->exited = WIFEXITED(status) ? 1 : 0;
            st->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
            st->term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            return 0;
        }
        if (w < 0 && errno == ECHILD)
            return -2;
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 10 * 1000 * 1000; /* 10ms */
            nanosleep(&ts, NULL);
        }
        waited += 10;
    }
    return -1;
}

/* Run the CPU workload inside `cg` for `ms` milliseconds and return the delta
 * of cpu.stat across the run. Returns 0 on success. */
static int run_cpu(const char *cg, const char *workload, int ms,
                   cpustat_t *delta, long long *wall_ms)
{
    cpustat_t before;
    cpustat_t after;
    char msbuf[32];
    const char *argv[4];
    process_exec_t spec;
    process_gate_t gate;
    process_status_t st;
    long pid;
    long long t0;
    int rc;

    if (read_cpu_stat(cg, &before) != 0)
        return -1;

    snprintf(msbuf, sizeof(msbuf), "%d", ms);
    argv[0] = "nexus_cpu_workload";
    argv[1] = "--milliseconds";
    argv[2] = msbuf;
    argv[3] = NULL;
    spec.path = workload;
    spec.argv = argv;
    spec.cwd = NULL;

    if (process_spawn_gated(&spec, NULL, &gate) < 0)
        return -2;

    rc = cgroup_attach(cg, gate.pid);
    if (rc != NEXUS_OK) {
        process_gate_abort(&gate);
        return -3;
    }

    t0 = now_ms();
    pid = process_gate_release(&gate);
    if (pid < 0)
        return -4;

    rc = wait_bounded(pid, ms + 10000, &st);
    *wall_ms = now_ms() - t0;
    if (rc == -1) {
        (void)process_stop(pid); /* guarantee cleanup */
        return -5;
    }

    if (read_cpu_stat(cg, &after) != 0)
        return -6;

    delta->usage_usec = after.usage_usec - before.usage_usec;
    delta->nr_periods = after.nr_periods - before.nr_periods;
    delta->nr_throttled = after.nr_throttled - before.nr_throttled;
    delta->throttled_usec = after.throttled_usec - before.throttled_usec;
    return 0;
}

#endif /* !_WIN32 */

int main(void)
{
    printf("=== NexusDB cgroup v2 CPU enforcement test (P4) ===\n\n");

#ifdef _WIN32
    (void)g_failures;
    printf("ENV-LIMITED: cgroup v2 CPU enforcement is Linux-only; Windows uses "
           "the cgroup shim.\n");
    printf("\n=== ENV-LIMITED (0 failure(s)) ===\n");
    return 0;
#else
    {
        char base_name[NEXUS_PATH_MAX];
        char lim_name[NEXUS_PATH_MAX];
        char base[NEXUS_PATH_MAX];
        char lim[NEXUS_PATH_MAX];
        const char *workload = getenv("NEXUS_CPU_WORKLOAD");
        const int ms = 800;
        const unsigned long mb = 256UL * 1024 * 1024;
        cpustat_t d_base[2];
        cpustat_t d_lim[2];
        long long w_base[2];
        long long w_lim[2];
        int i;
        int rc;

        if (workload == NULL || workload[0] == '\0')
            workload = "./nexus_cpu_workload";

        rc = cgroup_init(NULL);
        if (rc != NEXUS_OK) {
            printf("ENV-LIMITED: cgroup_init failed (rc=%d); need a writable "
                   "cgroup v2 root (delegated session + NEXUS_CGROUP_ROOT).\n",
                   rc);
            return 0;
        }
        snprintf(base_name, sizeof(base_name), "nexus_p4_cpu_base_%d",
                 getpid_compat());
        snprintf(lim_name, sizeof(lim_name), "nexus_p4_cpu_lim_%d",
                 getpid_compat());

        if (cgroup_create(base_name, base, sizeof(base)) != NEXUS_OK) {
            printf("ENV-LIMITED: cannot create cgroup under %s.\n",
                   cgroup_root());
            return 0;
        }
        if (cgroup_create(lim_name, lim, sizeof(lim)) != NEXUS_OK) {
            cgroup_destroy(base);
            printf("ENV-LIMITED: cannot create throttled cgroup.\n");
            return 0;
        }

        if (!is_real_cgroup(base) || !is_real_cgroup(lim)) {
            printf("ENV-LIMITED: %s is not a real cgroup2 hierarchy "
                   "(files cpu.stat/memory.max/cgroup.controllers absent).\n",
                   cgroup_root());
            cgroup_destroy(base);
            cgroup_destroy(lim);
            return 0;
        }

        if (access(workload, X_OK) != 0) {
            printf("ENV-LIMITED: CPU workload '%s' is not executable "
                   "(set NEXUS_CPU_WORKLOAD).\n", workload);
            cgroup_destroy(base);
            cgroup_destroy(lim);
            return 0;
        }

        printf("cgroup root : %s\n", cgroup_root());
        printf("workload    : %s\n", workload);
        printf("unrestricted: %s (cpu.max = max)\n", base);
        printf("throttled   : %s (cpu.max = 20%% of one CPU)\n\n", lim);

        rc = cgroup_set_limits(base, mb, 0); /* cpu.max = "max 100000" */
        CHECK("unrestricted cgroup configured", rc == NEXUS_OK);
        rc = cgroup_set_limits(lim, mb, 20); /* cpu.max = "20000 100000" */
        CHECK("throttled cgroup configured (20%)", rc == NEXUS_OK);

        for (i = 0; i < 2; i++) {
            rc = run_cpu(base, workload, ms, &d_base[i], &w_base[i]);
            CHECK("unrestricted run completed", rc == 0);
            rc = run_cpu(lim, workload, ms, &d_lim[i], &w_lim[i]);
            CHECK("throttled run completed", rc == 0);

            printf("--- run %d ---\n", i + 1);
            printf("  unrestricted: usage_usec=%llu nr_periods=%llu "
                   "nr_throttled=%llu throttled_usec=%llu wall_ms=%lld\n",
                   d_base[i].usage_usec, d_base[i].nr_periods,
                   d_base[i].nr_throttled, d_base[i].throttled_usec,
                   w_base[i]);
            printf("  throttled   : usage_usec=%llu nr_periods=%llu "
                   "nr_throttled=%llu throttled_usec=%llu wall_ms=%lld\n",
                   d_lim[i].usage_usec, d_lim[i].nr_periods,
                   d_lim[i].nr_throttled, d_lim[i].throttled_usec,
                   w_lim[i]);

            CHECK("unrestricted run burned CPU (usage_usec > 0)",
                  d_base[i].usage_usec > 0);
            CHECK("unrestricted run was not throttled (nr_throttled == 0)",
                  d_base[i].nr_throttled == 0);
            CHECK("throttled run hit the quota (nr_throttled >= 1)",
                  d_lim[i].nr_throttled >= 1);
            CHECK("throttled run accumulated throttle time (throttled_usec > 0)",
                  d_lim[i].throttled_usec > 0);
            CHECK("throttled run used materially less CPU than unrestricted",
                  d_base[i].usage_usec > 0 &&
                  d_lim[i].usage_usec < d_base[i].usage_usec * 6 / 10);
        }

        cgroup_destroy(base);
        cgroup_destroy(lim);

        printf("\n=== %s (%d failure(s)) ===\n",
               g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
               g_failures);
        return g_failures == 0 ? 0 : 1;
    }
#endif
}
