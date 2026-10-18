/*
 * NexusDB — cgroup v2 memory enforcement test (P4).
 *
 * Proves memory.max is actually enforced by the kernel and cleanly
 * distinguishes the three possible outcomes when a workload exceeds a limit:
 *
 *   1. cgroup enforcement  — the kernel OOM-kills the workload (killed by
 *                            SIGKILL) and increments memory.events oom_kill /
 *                            max for the cgroup.
 *   2. process alloc failure — malloc() returns NULL and the workload exits 4;
 *                            this is NOT cgroup enforcement (shown with a
 *                            single impossibly-large allocation).
 *   3. within limit        — the workload completes normally (exit 0) and no
 *                            memory.events counter moves.
 *
 * The workload is attached to the cgroup while still blocked before exec (via
 * process_spawn_gated), and swap is disabled in the test cgroup so the limit
 * cannot be silently absorbed. The limit is deliberately small (64 MiB) so
 * only the test's own child is ever a candidate for the OOM killer.
 *
 * Requires a real, writable cgroup v2 hierarchy (delegated systemd user session
 * with NEXUS_CGROUP_ROOT). Otherwise it reports ENV-LIMITED and exits 0.
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
    unsigned long long high;
    unsigned long long max;
    unsigned long long oom;
    unsigned long long oom_kill;
} memevents_t;

static void path_join(char *buf, size_t len, const char *dir, const char *file)
{
    snprintf(buf, len, "%s/%s", dir, file);
}

static int read_mem_events(const char *cg, memevents_t *out)
{
    char p[NEXUS_PATH_MAX + 32];
    FILE *f;
    char line[256];

    memset(out, 0, sizeof(*out));
    path_join(p, sizeof(p), cg, "memory.events");
    f = fopen(p, "r");
    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long v;
        if (sscanf(line, "high %llu", &v) == 1)
            out->high = v;
        else if (sscanf(line, "max %llu", &v) == 1)
            out->max = v;
        else if (sscanf(line, "oom %llu", &v) == 1)
            out->oom = v;
        else if (sscanf(line, "oom_kill %llu", &v) == 1)
            out->oom_kill = v;
    }
    fclose(f);
    return 0;
}

static unsigned long long read_ulong_file(const char *cg, const char *file)
{
    char p[NEXUS_PATH_MAX + 32];
    FILE *f;
    unsigned long long v = 0;

    path_join(p, sizeof(p), cg, file);
    f = fopen(p, "r");
    if (f == NULL)
        return 0;
    if (fscanf(f, "%llu", &v) != 1)
        v = 0;
    fclose(f);
    return v;
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

static int is_real_cgroup(const char *cg)
{
    return file_exists(cg, "cgroup.controllers") &&
           file_exists(cg, "memory.max") &&
           file_exists(cg, "memory.events") &&
           file_exists(cg, "memory.current");
}

static int write_str(const char *cg, const char *file, const char *val)
{
    char p[NEXUS_PATH_MAX + 32];
    FILE *f;

    path_join(p, sizeof(p), cg, file);
    f = fopen(p, "w");
    if (f == NULL)
        return -1;
    if (fprintf(f, "%s\n", val) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Reap pid within timeout_ms. 0 = reaped, -1 = timeout, -2 = already reaped. */
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
            ts.tv_nsec = 10 * 1000 * 1000;
            nanosleep(&ts, NULL);
        }
        waited += 10;
    }
    return -1;
}

/* Run the memory workload in `cg`; report how it ended and the memory.events
 * delta across the run. 0 on success (a reaped child, regardless of outcome). */
static int run_mem(const char *cg, const char *workload,
                   unsigned long long bytes, unsigned long long chunk,
                   int timeout_ms, process_status_t *st,
                   memevents_t *delta, unsigned long long *current_after)
{
    memevents_t before;
    memevents_t after;
    char bbuf[32];
    char cbuf[32];
    const char *argv[6];
    process_exec_t spec;
    process_gate_t gate;
    long pid;
    int rc;

    if (read_mem_events(cg, &before) != 0)
        return -1;

    snprintf(bbuf, sizeof(bbuf), "%llu", bytes);
    snprintf(cbuf, sizeof(cbuf), "%llu", chunk);
    argv[0] = "nexus_mem_workload";
    argv[1] = "--bytes";
    argv[2] = bbuf;
    argv[3] = "--chunk";
    argv[4] = cbuf;
    argv[5] = NULL;
    spec.path = workload;
    spec.argv = argv;
    spec.cwd = NULL;

    if (process_spawn_gated(&spec, NULL, &gate) < 0)
        return -2;
    if (cgroup_attach(cg, gate.pid) != NEXUS_OK) {
        process_gate_abort(&gate);
        return -3;
    }
    pid = process_gate_release(&gate);
    if (pid < 0)
        return -4;

    rc = wait_bounded(pid, timeout_ms, st);
    if (rc == -1) {
        (void)process_stop(pid);
        return -5;
    }
    if (rc == -2)
        return -6;

    if (read_mem_events(cg, &after) != 0)
        return -7;
    delta->high = after.high - before.high;
    delta->max = after.max - before.max;
    delta->oom = after.oom - before.oom;
    delta->oom_kill = after.oom_kill - before.oom_kill;
    *current_after = read_ulong_file(cg, "memory.current");
    return 0;
}

#endif /* !_WIN32 */

int main(void)
{
    printf("=== NexusDB cgroup v2 memory enforcement test (P4) ===\n\n");

#ifdef _WIN32
    (void)g_failures;
    printf("ENV-LIMITED: cgroup v2 memory enforcement is Linux-only; Windows "
           "uses the cgroup shim.\n");
    printf("\n=== ENV-LIMITED (0 failure(s)) ===\n");
    return 0;
#else
    {
        const unsigned long long limit = 64ULL * 1024 * 1024;      /* 64 MiB */
        const unsigned long long over = 256ULL * 1024 * 1024;      /* 256 MiB */
        const unsigned long long under = 16ULL * 1024 * 1024;      /* 16 MiB */
        const unsigned long long impossible = 1ULL << 48;          /* 256 TiB */
        const unsigned long long chunk = 4ULL * 1024 * 1024;
        char name[NEXUS_PATH_MAX];
        char cg[NEXUS_PATH_MAX];
        const char *workload = getenv("NEXUS_MEM_WORKLOAD");
        process_status_t st;
        memevents_t delta;
        unsigned long long current_after;
        int rc;

        if (workload == NULL || workload[0] == '\0')
            workload = "./nexus_mem_workload";

        rc = cgroup_init(NULL);
        if (rc != NEXUS_OK) {
            printf("ENV-LIMITED: cgroup_init failed (rc=%d); need a writable "
                   "cgroup v2 root (delegated session + NEXUS_CGROUP_ROOT).\n",
                   rc);
            return 0;
        }

        snprintf(name, sizeof(name), "nexus_p4_mem_%d", getpid_compat());
        if (cgroup_create(name, cg, sizeof(cg)) != NEXUS_OK) {
            printf("ENV-LIMITED: cannot create cgroup under %s.\n",
                   cgroup_root());
            return 0;
        }

        if (!is_real_cgroup(cg)) {
            printf("ENV-LIMITED: %s is not a real cgroup2 hierarchy "
                   "(memory.max/events absent).\n", cgroup_root());
            cgroup_destroy(cg);
            return 0;
        }

        if (access(workload, X_OK) != 0) {
            printf("ENV-LIMITED: memory workload '%s' is not executable "
                   "(set NEXUS_MEM_WORKLOAD).\n", workload);
            cgroup_destroy(cg);
            return 0;
        }

        rc = cgroup_set_limits(cg, (unsigned long)limit, 0);
        CHECK("memory limit configured (64 MiB)", rc == NEXUS_OK);
        CHECK("memory.max read back", read_ulong_file(cg, "memory.max") == limit);

        /* Disable swap so the limit cannot be silently absorbed. The kernel
         * OOM-kills instead of swapping to satisfy the charge. */
        if (write_str(cg, "memory.swap.max", "0") != 0) {
            printf("ENV-LIMITED: cannot set memory.swap.max=0; OOM semantics "
                   "not guaranteed here.\n");
            cgroup_destroy(cg);
            return 0;
        }
        printf("cgroup      : %s\n", cg);
        printf("workload    : %s\n", workload);
        printf("memory.max  : %llu bytes; memory.swap.max = 0\n\n", limit);

        /* --- Case 1: exceed the limit -> cgroup OOM kill. --- */
        printf("--- case 1: allocate %llu MiB with a %llu MiB limit ---\n",
               over / (1024 * 1024), limit / (1024 * 1024));
        rc = run_mem(cg, workload, over, chunk, 15000, &st, &delta,
                     &current_after);
        CHECK("over-limit run reaped", rc == 0);
        printf("  killed_by_signal=%d signal=%d exit=%d exit_code=%d\n",
               !st.exited, st.term_signal, st.exited, st.exit_code);
        printf("  memory.events delta: max=%llu oom=%llu oom_kill=%llu "
               "current_after=%llu\n",
               delta.max, delta.oom, delta.oom_kill, current_after);
        CHECK("over-limit workload was OOM-killed (SIGKILL)",
              !st.exited && st.term_signal == SIGKILL);
        CHECK("memory.events oom_kill incremented",
              delta.oom_kill >= 1);
        CHECK("memory.events max (limit hits) incremented",
              delta.max >= 1);

        /* --- Case 2: stay within the limit -> clean exit, no OOM. --- */
        printf("\n--- case 2: allocate %llu MiB (within limit) ---\n",
               under / (1024 * 1024));
        rc = run_mem(cg, workload, under, chunk, 15000, &st, &delta,
                     &current_after);
        CHECK("within-limit run reaped", rc == 0);
        printf("  exited=%d exit_code=%d oom_kill_delta=%llu\n",
               st.exited, st.exit_code, delta.oom_kill);
        CHECK("within-limit workload completed (exit 0)",
              st.exited && st.exit_code == 0);
        CHECK("within-limit run triggered no OOM kill", delta.oom_kill == 0);

        /* --- Case 3: impossible single allocation -> alloc failure, not OOM.
         * This shows the test distinguishes a manual malloc failure (exit 4)
         * from kernel enforcement (SIGKILL). --- */
        printf("\n--- case 3: single %llu TiB allocation (alloc failure) ---\n",
               impossible / (1024ULL * 1024 * 1024 * 1024));
        rc = run_mem(cg, workload, impossible, impossible, 5000, &st, &delta,
                     &current_after);
        CHECK("alloc-failure run reaped", rc == 0);
        printf("  exited=%d exit_code=%d oom_kill_delta=%llu\n",
               st.exited, st.exit_code, delta.oom_kill);
        CHECK("impossible allocation failed in-process (exit 4, not OOM)",
              st.exited && st.exit_code == 4);
        CHECK("alloc failure triggered no cgroup OOM kill",
              delta.oom_kill == 0);

        cgroup_destroy(cg);

        printf("\n=== %s (%d failure(s)) ===\n",
               g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
               g_failures);
        return g_failures == 0 ? 0 : 1;
    }
#endif
}
