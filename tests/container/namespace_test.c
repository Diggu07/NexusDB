/*
 * NexusDB — Linux namespace integration test (P2).
 *
 * Verifies that requested namespace isolation is applied in the gated child
 * BEFORE the workload executes, using direct kernel-level checks:
 *   - /proc/<pid>/ns/<type> inode comparison against this process
 *   - /proc/<pid>/status NSpid listing (2 entries == new PID namespace)
 *
 * Privilege note: creating PID/NET/UTS/IPC namespaces needs CAP_SYS_ADMIN.
 * Run this binary under a user namespace to grant it, e.g.
 *     unshare -U --map-root-user --mount-proc ./nexus_namespace_test
 * When the capability is absent the isolation checks are reported as
 * ENV-LIMITED (never counted as passed) and only the denial/failure semantics
 * are asserted.
 *
 * Exit 0 on success, 1 on any failure. Windows: asserts the explicit
 * "unsupported" contract ("never a false success").
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "container/container.h"
#include "container/namespace.h"
#include "container/process.h"

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int g_failures = 0;
static int g_skipped = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", (desc)); } \
    else { printf("FAIL: %s\n", (desc)); g_failures++; } \
} while (0)

#define SKIP(desc) do { \
    printf("ENV-LIMITED: %s\n", (desc)); g_skipped++; \
} while (0)

#ifndef _WIN32
static int test_ns_prepare(void *arg)
{
    const nexus_namespace_config_t *cfg =
        (const nexus_namespace_config_t *)arg;
    if (namespace_configure_child(cfg) != NEXUS_OK)
        return errno != 0 ? errno : EPERM;
    return 0;
}

static void reset_gate(process_gate_t *g)
{
    g->pid = -1;
    g->release_fd = -1;
    g->status_fd = -1;
    g->stack = NULL;
    g->stack_size = 0;
}

static void read_ns(char *out, size_t out_len, const char *path)
{
    ssize_t n;
    out[0] = '\0';
    n = readlink(path, out, out_len - 1);
    if (n > 0)
        out[n] = '\0';
}

/* Compare /proc/<pid>/ns/<type> against this process: isolation means the
 * namespace inode differs. */
static void check_ns_differs(const char *type, long pid)
{
    char self[128];
    char child[128];
    char self_path[64];
    char child_path[64];
    char desc[512];

    snprintf(self_path, sizeof(self_path), "/proc/self/ns/%s", type);
    snprintf(child_path, sizeof(child_path), "/proc/%ld/ns/%s", pid, type);
    read_ns(self, sizeof(self), self_path);
    read_ns(child, sizeof(child), child_path);

    snprintf(desc, sizeof(desc), "child %s namespace differs from host (%s vs %s)",
             type, self, child);
    CHECK(desc, self[0] != '\0' && child[0] != '\0' && strcmp(self, child) != 0);
}

/* True if /proc/<pid>/status NSpid lists >= 2 pids (host + inner). */
static int nspid_depth(long pid)
{
    char path[64];
    char line[256];
    FILE *f;
    int depth = 0;

    snprintf(path, sizeof(path), "/proc/%ld/status", pid);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "NSpid:", 6) == 0) {
            const char *p = line + 6;
            while (*p != '\0') {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p >= '0' && *p <= '9') {
                    depth++;
                    while (*p >= '0' && *p <= '9')
                        p++;
                } else {
                    break;
                }
            }
            break;
        }
    }
    fclose(f);
    return depth;
}
#endif /* !_WIN32 */

int main(void)
{
    nexus_namespace_config_t ns;
    nexus_namespace_config_t off;

    printf("=== NexusDB namespace integration test (P2) ===\n\n");

    /* 1. Default configuration requests all isolation types. */
    namespace_config_default(&ns, "nxtest");
    CHECK("default config isolates PID", ns.isolate_pid == 1);
    CHECK("default config isolates mount", ns.isolate_mount == 1);
    CHECK("default config isolates net", ns.isolate_net == 1);
    CHECK("default config isolates UTS", ns.isolate_uts == 1);
    CHECK("default config isolates IPC", ns.isolate_ipc == 1);
    CHECK("default hostname is the container name",
          ns.hostname != NULL && strcmp(ns.hostname, "nxtest") == 0);

    /* 2. clone-flag translation. */
    memset(&off, 0, sizeof(off));
    off.hostname = NULL;
    CHECK("NULL config -> 0 clone flags", namespace_clone_flags(NULL) == 0);
    CHECK("no isolation requested -> 0 clone flags",
          namespace_clone_flags(&off) == 0);
#ifndef _WIN32
    CHECK("all isolation requested -> non-zero clone flags",
          namespace_clone_flags(&ns) != 0);
    {
        nexus_namespace_config_t sel;
        memset(&sel, 0, sizeof(sel));
        sel.isolate_pid = 1;
        CHECK("pid-only config -> non-zero clone flags",
              namespace_clone_flags(&sel) != 0);
    }
#endif

    /* 3. configure_child with nothing requested is a no-op. */
    CHECK("configure_child(NULL) ok", namespace_configure_child(NULL) == NEXUS_OK);
    CHECK("configure_child(no isolation) ok",
          namespace_configure_child(&off) == NEXUS_OK);

#ifdef _WIN32
    /* Windows must report isolation as unsupported, never a false success. */
    CHECK("Windows configure_child(isolation) -> UNSUPPORTED",
          namespace_configure_child(&ns) == NEXUS_ERR_UNSUPPORTED);
    CHECK("Windows namespace_apply(isolation) -> UNSUPPORTED",
          namespace_apply(&ns) == NEXUS_ERR_UNSUPPORTED);
    printf("\n=== %s (%d failure(s), %d env-limited) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
           g_skipped);
    return g_failures == 0 ? 0 : 1;
#else
    /* 4. End-to-end gated spawn with full isolation (privilege-dependent). */
    {
        process_exec_t spec;
        process_gate_t gate;
        process_gate_config_t gc;
        static const char *const sargv[] = { "sleep", "30", NULL };
        long pid;

        spec.path = "/bin/sleep";
        spec.argv = sargv;
        spec.cwd = NULL;

        gc.clone_flags = namespace_clone_flags(&ns);
        gc.child_prepare = test_ns_prepare;
        gc.child_arg = &ns;
        reset_gate(&gate);

        errno = 0;
        pid = process_spawn_gated(&spec, &gc, &gate);
        if (pid <= 0) {
            SKIP("create PID/NET/UTS/IPC namespaces denied (need CAP_SYS_ADMIN; "
                 "run under `unshare -U --map-root-user`)");
            CHECK("denied namespace spawn created no child (EPERM/EINVAL)",
                  errno == EPERM || errno == EINVAL);
            {
                int wst;
                pid_t w = waitpid(-1, &wst, WNOHANG);
                CHECK("no zombie after denied namespace spawn",
                      w == -1 && errno == ECHILD);
            }
        } else {
            CHECK("namespaced gated child -> pid > 0", pid > 0);
            CHECK("namespaced child alive while blocked",
                  process_is_alive(pid) != 0);

            CHECK("release namespaced child -> returns pid",
                  process_gate_release(&gate) == pid);
            CHECK("namespaced child alive after release (workload running)",
                  process_is_alive(pid) != 0);

            /* Direct kernel-level isolation proof for the RUNNING workload. */
            check_ns_differs("mnt", pid);
            check_ns_differs("pid", pid);
            check_ns_differs("net", pid);
            check_ns_differs("uts", pid);
            check_ns_differs("ipc", pid);

            {
                int depth = nspid_depth(pid);
                CHECK("workload is PID 1 in a new PID namespace (NSpid depth 2)",
                      depth == 2);
            }

            CHECK("stop namespaced child -> 0", process_stop(pid) == 0);
            CHECK("namespaced child gone after stop", process_is_alive(pid) == 0);
        }
    }

    /* 5. Namespace SETUP failure (after clone) must block execution and reap.
     * A >64-byte UTS hostname makes sethostname() fail (EINVAL) in the child;
     * an over-long marker workload proves the workload never ran. */
    {
        char longhost[128];
        const char *marker = "/tmp/nexus_p2_ns_marker";
        process_exec_t spec;
        process_gate_t gate;
        process_gate_config_t gc;
        static const char *const targv[] = {
            "touch", "/tmp/nexus_p2_ns_marker", NULL
        };
        nexus_namespace_config_t bad;
        long pid;

        (void)unlink(marker);
        memset(longhost, 'h', sizeof(longhost) - 1);
        longhost[sizeof(longhost) - 1] = '\0';

        memset(&bad, 0, sizeof(bad));
        bad.isolate_uts = 1;
        bad.hostname = longhost; /* 127 bytes > __NEW_UTS_LEN (64) */

        gc.clone_flags = namespace_clone_flags(&bad);
        gc.child_prepare = test_ns_prepare;
        gc.child_arg = &bad;
        reset_gate(&gate);

        spec.path = "touch";
        spec.argv = targv;
        spec.cwd = NULL;

        errno = 0;
        pid = process_spawn_gated(&spec, &gc, &gate);
        if (pid <= 0) {
            /* Namespace denied before a child existed (unprivileged): still a
             * hard namespace-setup failure; nothing executed. */
            CHECK("denied UTS namespace setup -> no child (EPERM/EINVAL)",
                  errno == EPERM || errno == EINVAL);
        } else {
            errno = 0;
            CHECK("child-side namespace setup failure -> -1/EINVAL",
                  process_gate_release(&gate) == -1 &&
                  (errno == EINVAL || errno == EPERM));
        }
        CHECK("workload did NOT execute when namespace setup failed",
              access(marker, F_OK) != 0);
        {
            int wst;
            pid_t w = waitpid(-1, &wst, WNOHANG);
            CHECK("no zombie after namespace setup failure",
                  w == -1 && errno == ECHILD);
        }
        (void)unlink(marker);
    }

    printf("\n=== %s (%d failure(s), %d env-limited) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
           g_skipped);
    return g_failures == 0 ? 0 : 1;
#endif
}
