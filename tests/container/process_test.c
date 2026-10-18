/*
 * NexusDB — direct process-execution API test (P0).
 *
 * Exercises process_spawn_exec() / process_wait() without touching the
 * container lifecycle:
 *   - successful exec of a known binary, live pid, stop
 *   - normal exit codes (0 and non-zero)
 *   - signal termination reported via term_signal
 *   - startup failure (missing executable, bad working directory) returns an
 *     error and leaves no live child or zombie
 *   - argument validation
 *
 * POSIX only. On Windows process_spawn_exec() is deliberately unimplemented
 * (process_spawn_placeholder() is the supported shim), so this test asserts
 * the documented -1/ENOSYS contract there.
 *
 * Exit 0 on success, 1 on any failure. Same PASS/FAIL style as the sibling
 * container tests.
 */
/* POSIX declarations (kill, waitpid, pid_t) must be exposed before any system
 * header is included under -std=c11 (strict ANSI). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "container/process.h"

#ifdef _WIN32
#define NEXUS_TEST_CMD "C:\\Windows\\System32\\cmd.exe"
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* Child-side gate hooks used by section 12. */
static int g_hook_sentinel;

static int hook_ok(void *arg) { (void)arg; return 0; }
static int hook_fail(void *arg) { (void)arg; return EACCES; }
static int hook_check_arg(void *arg)
{
    return (arg == (void *)&g_hook_sentinel) ? 0 : EINVAL;
}
#endif

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", (desc)); } \
    else { printf("FAIL: %s\n", (desc)); g_failures++; } \
} while (0)

int main(void)
{
    process_exec_t spec;

    printf("=== NexusDB process execution test (P0) ===\n\n");

#ifdef _WIN32
    spec.path = NEXUS_TEST_CMD;
    spec.argv = NULL;
    spec.cwd = NULL;
    errno = 0;
    CHECK("spawn_exec is POSIX-only -> -1/ENOSYS",
          process_spawn_exec(&spec) == -1 && errno == ENOSYS);
    {
        process_gate_t gate;
        errno = 0;
        CHECK("gated spawn is POSIX-only -> -1/ENOSYS",
              process_spawn_gated(&spec, NULL, &gate) == -1 && errno == ENOSYS);
        process_gate_abort(&gate); /* safe no-op on Windows */
    }
#else
    process_status_t st;
    long pid;

    /* 1. Argument validation. */
    errno = 0;
    CHECK("NULL spec -> -1/EINVAL",
          process_spawn_exec(NULL) == -1 && errno == EINVAL);
    spec.path = "";
    spec.argv = NULL;
    spec.cwd = NULL;
    errno = 0;
    CHECK("empty path -> -1/EINVAL",
          process_spawn_exec(&spec) == -1 && errno == EINVAL);

    /* 2. Successful launch: live pid + normal exit status 0. */
    {
        static const char *const yes[] = { "true", NULL };
        spec.path = "/bin/true";
        spec.argv = yes;
        spec.cwd = NULL;
        errno = 0;
        pid = process_spawn_exec(&spec);
        CHECK("launch /bin/true -> pid > 0", pid > 0);
        if (pid > 0) {
            CHECK("/bin/true visible before wait", process_is_alive(pid) != 0);
            memset(&st, 0x7f, sizeof(st));
            CHECK("wait /bin/true -> ok", process_wait(pid, &st) == 0);
            CHECK("/bin/true exited normally", st.exited == 1);
            CHECK("/bin/true exit code is 0", st.exit_code == 0);
            errno = 0;
            CHECK("second wait -> -1/ECHILD (already reaped)",
                  process_wait(pid, &st) == -1 && errno == ECHILD);
        }
    }

    /* 3. Non-zero exit code is preserved (not silently discarded). */
    {
        static const char *const no[] = { "false", NULL };
        spec.path = "/bin/false";
        spec.argv = no;
        spec.cwd = NULL;
        pid = process_spawn_exec(&spec);
        CHECK("launch /bin/false -> pid > 0", pid > 0);
        if (pid > 0) {
            CHECK("wait /bin/false -> ok", process_wait(pid, &st) == 0);
            CHECK("/bin/false exit code is 1",
                  st.exited == 1 && st.exit_code == 1);
        }
    }

    /* 4. Signal termination is reported as not-exited + terminating signal. */
    {
        static const char *const snooze[] = { "sleep", "30", NULL };
        spec.path = "/bin/sleep";
        spec.argv = snooze;
        spec.cwd = NULL;
        pid = process_spawn_exec(&spec);
        CHECK("launch /bin/sleep -> pid > 0", pid > 0);
        if (pid > 0) {
            CHECK("sleep child is alive", process_is_alive(pid) != 0);
            (void)kill((pid_t)pid, SIGKILL);
            CHECK("wait killed child -> ok", process_wait(pid, &st) == 0);
            CHECK("reported as signal termination",
                  st.exited == 0 && st.term_signal == SIGKILL);
        }
    }

    /* 5. Startup failure: missing executable -> error, no zombie. */
    errno = 0;
    spec.path = "/nonexistent/nexusdb-no-such-binary";
    spec.argv = NULL;
    spec.cwd = NULL;
    CHECK("missing executable -> -1/ENOENT",
          process_spawn_exec(&spec) == -1 && errno == ENOENT);
    {
        int wst;
        pid_t w = waitpid(-1, &wst, WNOHANG);
        CHECK("no zombie after missing-executable failure",
              w == -1 && errno == ECHILD);
    }

    /* 6. Startup failure: bad working directory surfaces the child errno. */
    errno = 0;
    spec.path = "/bin/true";
    spec.argv = NULL;
    spec.cwd = "/nonexistent/nexusdb-cwd";
    CHECK("bad cwd -> -1/ENOENT",
          process_spawn_exec(&spec) == -1 && errno == ENOENT);
    {
        int wst;
        pid_t w = waitpid(-1, &wst, WNOHANG);
        CHECK("no zombie after bad-cwd failure", w == -1 && errno == ECHILD);
    }

    /* 7. Stop a live child (lifecycle-compatible path). */
    {
        static const char *const longsleep[] = { "sleep", "30", NULL };
        spec.path = "/bin/sleep";
        spec.argv = longsleep;
        spec.cwd = NULL;
        pid = process_spawn_exec(&spec);
        CHECK("launch long child -> pid > 0", pid > 0);
        if (pid > 0) {
            CHECK("long child alive", process_is_alive(pid) != 0);
            CHECK("stop long child -> 0", process_stop(pid) == 0);
            CHECK("long child gone after stop", process_is_alive(pid) == 0);
        }
    }

    /* 8. Gated spawn: the child is BLOCKED before exec. /proc/<pid>/exe still
     * identifies the test binary (not the target), and only after release does
     * it point at the target -- the attach-before-exec guarantee, observed. */
    {
        static const char *const gsleep[] = { "sleep", "30", NULL };
        process_gate_t gate;
        long gpid;
        char linkbuf[256];
        char path[64];
        ssize_t ln;

        gate.pid = -1;
        gate.release_fd = -1;
        gate.status_fd = -1;
        spec.path = "/bin/sleep";
        spec.argv = gsleep;
        spec.cwd = NULL;

        gpid = process_spawn_gated(&spec, NULL, &gate);
        CHECK("gated spawn /bin/sleep -> pid > 0", gpid > 0);
        if (gpid > 0) {
            CHECK("gated child alive while blocked",
                  process_is_alive(gpid) != 0);
            CHECK("gate holds a release + status fd",
                  gate.release_fd >= 0 && gate.status_fd >= 0);

            snprintf(path, sizeof(path), "/proc/%ld/exe", gpid);
            ln = readlink(path, linkbuf, sizeof(linkbuf) - 1);
            if (ln > 0) {
                linkbuf[ln] = '\0';
                CHECK("child has NOT exec'd before release",
                      strstr(linkbuf, "/sleep") == NULL);
            } else {
                printf("NOTE: /proc/%ld/exe unavailable; skipped pre-exec probe\n",
                       gpid);
            }

            CHECK("release gated child -> returns pid",
                  process_gate_release(&gate) == gpid);
            CHECK("gate reset after release",
                  gate.pid == -1 && gate.release_fd == -1 &&
                  gate.status_fd == -1);

            ln = readlink(path, linkbuf, sizeof(linkbuf) - 1);
            if (ln > 0) {
                linkbuf[ln] = '\0';
                CHECK("child HAS exec'd after release",
                      strstr(linkbuf, "/sleep") != NULL);
            }
            CHECK("gated child alive after release",
                  process_is_alive(gpid) != 0);
            CHECK("stop released gated child -> 0", process_stop(gpid) == 0);
        }
    }

    /* 9. Gated startup failure is surfaced by release and reaped (no zombie). */
    {
        process_gate_t gate;
        long gpid;

        gate.pid = -1;
        gate.release_fd = -1;
        gate.status_fd = -1;
        spec.path = "/nonexistent/nexusdb-gated-nope";
        spec.argv = NULL;
        spec.cwd = NULL;

        gpid = process_spawn_gated(&spec, NULL, &gate);
        CHECK("gated spawn of bad path still forks -> pid > 0", gpid > 0);
        if (gpid > 0) {
            errno = 0;
            CHECK("release bad-exec child -> -1/ENOENT",
                  process_gate_release(&gate) == -1 && errno == ENOENT);
            CHECK("gate reset after failed release",
                  gate.pid == -1 && gate.release_fd == -1 &&
                  gate.status_fd == -1);
            {
                int wst;
                pid_t w = waitpid(-1, &wst, WNOHANG);
                CHECK("no zombie after gated exec failure",
                      w == -1 && errno == ECHILD);
            }
        }
    }

    /* 10. Abort kills the still-blocked child and reaps it (never execs). */
    {
        static const char *const asleep[] = { "sleep", "30", NULL };
        process_gate_t gate;
        long gpid;

        gate.pid = -1;
        gate.release_fd = -1;
        gate.status_fd = -1;
        spec.path = "/bin/sleep";
        spec.argv = asleep;
        spec.cwd = NULL;

        gpid = process_spawn_gated(&spec, NULL, &gate);
        CHECK("gated spawn for abort -> pid > 0", gpid > 0);
        if (gpid > 0) {
            process_gate_abort(&gate);
            CHECK("aborted child is gone", process_is_alive(gpid) == 0);
            CHECK("gate reset after abort",
                  gate.pid == -1 && gate.release_fd == -1 &&
                  gate.status_fd == -1);
            {
                int wst;
                pid_t w = waitpid((pid_t)gpid, &wst, WNOHANG);
                CHECK("aborted child reaped (ECHILD)",
                      w == -1 && errno == ECHILD);
            }
        }
        /* Abort on an already-idle gate is a safe no-op. */
        process_gate_abort(&gate);
        CHECK("abort on idle gate is a no-op",
              gate.pid == -1 && gate.release_fd == -1 &&
              gate.status_fd == -1);
        process_gate_abort(NULL);
    }

    /* 11. Gated argument validation. */
    {
        process_gate_t gate;
        process_gate_t idle;

        errno = 0;
        CHECK("gated NULL spec -> -1/EINVAL",
              process_spawn_gated(NULL, NULL, &gate) == -1 && errno == EINVAL);
        gate.pid = -1;
        gate.release_fd = -1;
        gate.status_fd = -1;
        spec.path = "";
        spec.argv = NULL;
        spec.cwd = NULL;
        errno = 0;
        CHECK("gated empty path -> -1/EINVAL",
              process_spawn_gated(&spec, NULL, &gate) == -1 && errno == EINVAL);

        idle.pid = -1;
        idle.release_fd = -1;
        idle.status_fd = -1;
        errno = 0;
        CHECK("release idle gate -> -1/EINVAL",
              process_gate_release(&idle) == -1 && errno == EINVAL);
        errno = 0;
        CHECK("release NULL gate -> -1/EINVAL",
              process_gate_release(NULL) == -1 && errno == EINVAL);
    }

    /* 12. Child-side gate hook (the namespace-integration seam, tested without
     * requiring privileges). The hook runs after release, before exec. */
    {
        process_gate_t gate;
        process_gate_config_t gc;
        long gpid;
        const char *marker = "/tmp/nexus_p2_hook_marker";

        (void)unlink(marker);

        /* 12a. Hook succeeds -> the workload executes. */
        gc.clone_flags = 0;
        gc.child_prepare = hook_ok;
        gc.child_arg = NULL;
        gate.pid = -1;
        gate.release_fd = -1;
        gate.status_fd = -1;
        gate.stack = NULL;
        gate.stack_size = 0;
        spec.path = "/bin/true";
        spec.argv = NULL;
        spec.cwd = NULL;
        gpid = process_spawn_gated(&spec, &gc, &gate);
        CHECK("hooked spawn -> pid > 0", gpid > 0);
        if (gpid > 0) {
            CHECK("hook success: release runs the workload",
                  process_gate_release(&gate) == gpid);
            CHECK("hook success: workload reaped",
                  process_wait(gpid, &st) == 0);
        }

        /* 12b. Hook fails -> the workload NEVER executes and the child is
         * reaped. A marker file the workload would create proves no exec. */
        gc.child_prepare = hook_fail;
        gc.child_arg = NULL;
        gate.pid = -1;
        gate.release_fd = -1;
        gate.status_fd = -1;
        gate.stack = NULL;
        gate.stack_size = 0;
        spec.path = "touch";
        {
            static const char *const targv[] = {
                "touch", "/tmp/nexus_p2_hook_marker", NULL
            };
            spec.argv = targv;
        }
        spec.cwd = NULL;
        gpid = process_spawn_gated(&spec, &gc, &gate);
        CHECK("hooked spawn (touch) -> pid > 0", gpid > 0);
        if (gpid > 0) {
            errno = 0;
            CHECK("hook failure -> release -1/EACCES",
                  process_gate_release(&gate) == -1 && errno == EACCES);
            CHECK("hook failure -> workload did NOT execute (no marker)",
                  access(marker, F_OK) != 0);
            {
                int wst;
                pid_t w = waitpid(-1, &wst, WNOHANG);
                CHECK("no zombie after hook failure",
                      w == -1 && errno == ECHILD);
            }
        }

        /* 12c. Hook receives its argument. */
        gc.child_prepare = hook_check_arg;
        gc.child_arg = (void *)&g_hook_sentinel;
        gate.pid = -1;
        gate.release_fd = -1;
        gate.status_fd = -1;
        gate.stack = NULL;
        gate.stack_size = 0;
        spec.path = "/bin/true";
        spec.argv = NULL;
        spec.cwd = NULL;
        gpid = process_spawn_gated(&spec, &gc, &gate);
        CHECK("hook-arg spawn -> pid > 0", gpid > 0);
        if (gpid > 0) {
            CHECK("hook received arg -> workload runs",
                  process_gate_release(&gate) == gpid);
            CHECK("hook-arg workload reaped", process_wait(gpid, &st) == 0);
        }

        (void)unlink(marker);
    }
#endif

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
