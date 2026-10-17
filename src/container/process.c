/*
 * NexusDB — child-process launcher (see process.h for design).
 */
#include "container/process.h"

#include "container/container.h"
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define NEXUS_MAX_TRACKED 64

/* pid -> HANDLE table so stop() can terminate + CloseHandle. */
static LONG g_pids[NEXUS_MAX_TRACKED];
static HANDLE g_handles[NEXUS_MAX_TRACKED];
static int g_used[NEXUS_MAX_TRACKED];

static void track_add(LONG pid, HANDLE h)
{
    int i;
    for (i = 0; i < NEXUS_MAX_TRACKED; i++) {
        if (!g_used[i]) {
            g_used[i] = 1;
            g_pids[i] = pid;
            g_handles[i] = h;
            return;
        }
    }
    /* Table full: handle leaks by design here is worse than not tracking,
     * but this only bounds the demo; close to avoid an HD leak. */
    CloseHandle(h);
}

static HANDLE track_take(LONG pid)
{
    int i;
    for (i = 0; i < NEXUS_MAX_TRACKED; i++) {
        if (g_used[i] && g_pids[i] == pid) {
            HANDLE h = g_handles[i];
            g_used[i] = 0;
            g_handles[i] = NULL;
            return h;
        }
    }
    return NULL;
}

/* Join the mock cgroup by recording pid (shim: plain file write). */
static int shim_attach(const char *cgroup_path, long pid)
{
    char fp[NEXUS_PATH_MAX + 32];
    FILE *f;
    if (!cgroup_path || cgroup_path[0] == '\0' || pid <= 0)
        return -1;
    snprintf(fp, sizeof(fp), "%s\\cgroup.procs", cgroup_path);
    f = fopen(fp, "w");
    if (!f)
        return -1;
    fprintf(f, "%ld\n", pid);
    if (fclose(f) != 0)
        return -1;
    return 0;
}

long process_spawn(const char *exe_path, char *const argv[],
                   const char *cgroup_path,
                   const nexus_namespace_config_t *ns_cfg)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *cmd = NULL;
    size_t need = 1;
    (void)ns_cfg; /* namespaces unsupported on the Windows shim */

    if (!exe_path || exe_path[0] == '\0' || !argv || !argv[0]) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    /* Build a mutable command line: exe + args, quoting args with spaces. */
    for (char *const *a = argv; *a; a++) {
        need += strlen(*a) + 3;
    }
    cmd = (char *)malloc(need);
    if (!cmd) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    cmd[0] = '\0';
    for (char *const *a = argv; *a; a++) {
        int has_space = strchr(*a, ' ') != NULL || strchr(*a, '\t') != NULL;
        if (a != argv)
            strcat(cmd, " ");
        if (has_space)
            strcat(cmd, "\"");
        strcat(cmd, *a);
        if (has_space)
            strcat(cmd, "\"");
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* Direct launch, no shell (no cmd.exe involved). */
    if (!CreateProcessA(exe_path, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(cmd);
        return -1;
    }
    free(cmd);

    /* A limit that was not applied must never be reported as success. */
    if (cgroup_path && cgroup_path[0] != '\0') {
        if (shim_attach(cgroup_path, (long)pi.dwProcessId) != 0) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SetLastError(ERROR_ACCESS_DENIED);
            return -1;
        }
    }

    CloseHandle(pi.hThread);
    track_add((LONG)pi.dwProcessId, pi.hProcess);
    return (long)pi.dwProcessId;
}

long process_spawn_placeholder(void)
{
    /* ~1h idle stand-in for the DB engine process. */
    static char arg0[] = "ping";
    static char arg1[] = "-n";
    static char arg2[] = "3600";
    static char arg3[] = "127.0.0.1";
    char *argv[] = {arg0, arg1, arg2, arg3, NULL};
    return process_spawn("C:\\Windows\\System32\\ping.exe", argv, NULL, NULL);
}

int process_stop(long pid)
{
    HANDLE h;
    DWORD code;

    if (pid <= 0)
        return -1;
    h = track_take(pid);
    if (h == NULL) {
        /* Unknown pid: try to open it (e.g. spawned before? no) — else fail. */
        h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
        if (h == NULL)
            return -1;
    }
    TerminateProcess(h, 0);
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
    (void)code;
    return 0;
}

int process_is_alive(long pid)
{
    HANDLE h;
    DWORD code;

    if (pid <= 0)
        return 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h == NULL)
        return 0;
    if (!GetExitCodeProcess(h, &code)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return code == STILL_ACTIVE;
}

#else /* Linux / POSIX */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "container/namespace.h"

#define NEXUS_SPAWN_TIMEOUT_MS 5000
#define NEXUS_SURVIVAL_POLL_MS 20
#define NEXUS_SURVIVAL_POLLS 3

/* Child-side cgroup join: the child records its OWN pid before exec, so
 * there is no window where it runs outside its limits. Implemented locally
 * (not via cgroup_attach) to keep this layer free of the container layer. */
static int self_attach(const char *cgroup_path)
{
    char fp[NEXUS_PATH_MAX + 32];
    FILE *f;
    if (!cgroup_path || cgroup_path[0] == '\0')
        return 0;
    snprintf(fp, sizeof(fp), "%s/cgroup.procs", cgroup_path);
    f = fopen(fp, "w");
    if (!f)
        return -1;
    fprintf(f, "%ld\n", (long)getpid());
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/* Error-pipe protocol: child writes stage byte + errno, then _exit(127).
 * Stages: 'N' namespace setup, 'C' cgroup join, 'E' exec. EOF with no bytes
 * means exec succeeded (write end is close-on-exec). */
static void child_fail(int wr, char stage, int errnum)
{
    char buf[1 + sizeof(int)];
    ssize_t left = sizeof(buf);
    buf[0] = stage;
    memcpy(buf + 1, &errnum, sizeof(int));
    while (left > 0) {
        ssize_t w = write(wr, buf + (sizeof(buf) - (size_t)left), (size_t)left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        left -= w;
    }
    _exit(127);
}

long process_spawn(const char *exe_path, char *const argv[],
                   const char *cgroup_path,
                   const nexus_namespace_config_t *ns_cfg)
{
    int errpipe[2] = {-1, -1};
    pid_t pid;

    if (!exe_path || exe_path[0] == '\0' || !argv || !argv[0]) {
        errno = EINVAL;
        return -1;
    }

    if (pipe(errpipe) != 0)
        return -1;
    /* Close-on-exec: successful exec closes errpipe[1] -> parent sees EOF. */
    (void)fcntl(errpipe[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid < 0) {
        int e = errno;
        close(errpipe[0]);
        close(errpipe[1]);
        errno = e;
        return -1;
    }
    if (pid == 0) {
        /* Child: never return; only _exit() or exec. No stdio output here
         * (parent buffers must not flush twice). */
        int ns_rc;
        close(errpipe[0]);

        if (ns_cfg) {
            ns_rc = namespace_apply(ns_cfg);
            if (ns_rc != 0) {
                int e = (ns_rc == NEXUS_ERR_INVALID) ? EINVAL : EPERM;
                child_fail(errpipe[1], 'N', e);
            }
        }

        if (cgroup_path && cgroup_path[0] != '\0') {
            if (self_attach(cgroup_path) != 0) {
                int e = errno;
                child_fail(errpipe[1], 'C', e);
            }
        }

        if (strchr(exe_path, '/') != NULL)
            execv(exe_path, argv);
        else
            execvp(exe_path, argv);
        {
            int e = errno; /* exec only returns on failure */
            child_fail(errpipe[1], 'E', e);
        }
    }

    /* Parent. */
    close(errpipe[1]);
    {
        struct pollfd pfd;
        int pr;
        pfd.fd = errpipe[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        do {
            pr = poll(&pfd, 1, NEXUS_SPAWN_TIMEOUT_MS);
        } while (pr < 0 && errno == EINTR);
        if (pr < 0) {
            int e = errno;
            close(errpipe[0]);
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            errno = e;
            return -1;
        }
        if (pr == 0) {
            /* Child stalled in setup: kill, reap, report timeout. */
            close(errpipe[0]);
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            errno = ETIMEDOUT;
            return -1;
        }
        {
            char buf[1 + sizeof(int)];
            ssize_t total = 0;
            ssize_t r;
            /* EOF (0 bytes, possibly after POLLHUP) means exec succeeded. */
            while (total < (ssize_t)sizeof(buf)) {
                r = read(errpipe[0], buf + total,
                         (size_t)((ssize_t)sizeof(buf) - total));
                if (r == 0)
                    break; /* EOF */
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                total += r;
            }
            close(errpipe[0]);
            if (total > 0) {
                /* Child reported a setup/exec failure: reap, forward errno. */
                int status = 0;
                int child_errno = EIO;
                if (total >= (ssize_t)(1 + sizeof(int)))
                    memcpy(&child_errno, buf + 1, sizeof(int));
                (void)waitpid(pid, &status, 0);
                errno = child_errno;
                return -1;
            }
        }
        /* EOF: exec succeeded. Survival check — this API is for long-lived
         * daemons; an instantly-exited child is a failed startup, and its
         * (reaped) pid must never be handed out (pid-reuse hazard). */
        for (int i = 0; i < NEXUS_SURVIVAL_POLLS; i++) {
            int status = 0;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                errno = ECHILD;
                return -1;
            }
            if (w < 0 && errno == ECHILD) {
                errno = ECHILD;
                return -1;
            }
            {
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = (long)NEXUS_SURVIVAL_POLL_MS * 1000 * 1000;
                nanosleep(&ts, NULL);
            }
        }
    }
    return (long)pid;
}

long process_spawn_placeholder(void)
{
    /* Long-lived idler via PATH lookup; honest failure (no in-process idle
     * fallback: a failed exec must surface, not masquerade as a child). */
    static char arg0[] = "sleep";
    static char arg1[] = "3600";
    char *argv[] = {arg0, arg1, NULL};
    return process_spawn("sleep", argv, NULL, NULL);
}

int process_stop(long pid)
{
    pid_t c = (pid_t)pid;
    int status;
    pid_t w;
    int tries;

    if (pid <= 0)
        return -1;

    /* Ask nicely first; ignore ESRCH (already gone). */
    if (kill(c, SIGTERM) < 0 && errno != ESRCH)
        return -1;

    /* Grace period ~2s, reaping if it exits. */
    for (tries = 0; tries < 20; tries++) {
        w = waitpid(c, &status, WNOHANG);
        if (w == c)
            return 0;               /* reaped */
        if (w < 0 && errno == ECHILD)
            return 0;               /* already reaped/gone */
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100 * 1000 * 1000; /* 100ms */
            nanosleep(&ts, NULL);
        }
    }

    /* Still there: force kill, then blocking reap. */
    (void)kill(c, SIGKILL);
    w = waitpid(c, &status, 0);
    if (w == c || (w < 0 && errno == ECHILD))
        return 0;
    return -1;
}

int process_is_alive(long pid)
{
    pid_t c = (pid_t)pid;
    if (pid <= 0)
        return 0;
    if (kill(c, 0) == 0)
        return 1; /* includes zombie children; stop() reaps them */
    return errno != ESRCH ? 1 : 0;
}

#endif
