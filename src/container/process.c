/*
 * NexusDB — placeholder child process (see process.h for design).
 */
/* POSIX declarations (kill, nanosleep, fork, waitpid, ...) must be exposed
 * before any system header is included: glibc hides them under -std=c11
 * (strict ANSI) unless a feature-test macro is defined first. _GNU_SOURCE also
 * exposes MSG_NOSIGNAL (used by the release channel so a child that died
 * before release can never raise SIGPIPE in the parent). */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "container/process.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdio.h>

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

long process_spawn_placeholder(void)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    /* ~1h idle stand-in for the DB engine process. */
    char cmd[] = "C:\\Windows\\System32\\ping.exe -n 3600 127.0.0.1";

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    track_add((LONG)pi.dwProcessId, pi.hProcess);
    return (long)pi.dwProcessId;
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

long process_spawn_exec(const process_exec_t *spec)
{
    /* Direct execution is POSIX-specific in P0. Windows keeps the existing
     * process_spawn_placeholder() shim, so fail loudly rather than pretend an
     * executable was launched. */
    (void)spec;
    errno = ENOSYS;
    return -1;
}

int process_wait(long pid, process_status_t *out)
{
    HANDLE h;
    DWORD code;

    if (pid <= 0)
        return -1;
    h = track_take(pid);
    if (h == NULL) {
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, (DWORD)pid);
        if (h == NULL)
            return -1;
    }
    WaitForSingleObject(h, INFINITE);
    if (!GetExitCodeProcess(h, &code)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    if (out != NULL) {
        out->exited = 1; /* Windows has no POSIX signal status */
        out->exit_code = (int)code;
        out->term_signal = 0;
    }
    return 0;
}

long process_spawn_gated(const process_exec_t *spec,
                         const process_gate_config_t *cfg,
                         process_gate_t *gate)
{
    /* Pre-exec gating + namespaces are POSIX-only (see the header). Windows
     * keeps the placeholder shim; fail loudly rather than pretend a gate with
     * namespaces exists. */
    (void)spec;
    (void)cfg;
    if (gate != NULL) {
        gate->pid = -1;
        gate->release_fd = -1;
        gate->status_fd = -1;
        gate->stack = NULL;
        gate->stack_size = 0;
    }
    errno = ENOSYS;
    return -1;
}

long process_gate_release(process_gate_t *gate)
{
    (void)gate;
    errno = ENOSYS;
    return -1;
}

void process_gate_abort(process_gate_t *gate)
{
    if (gate != NULL) {
        gate->pid = -1;
        gate->release_fd = -1;
        gate->status_fd = -1;
        gate->stack = NULL;
        gate->stack_size = 0;
    }
}

#else /* Linux / POSIX */

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Stack given to a namespace child created with clone(2). It is owned by the
 * gate and freed on release/abort, only once the child has exec'd or been
 * reaped. */
#define NEXUS_GATE_STACK (256 * 1024)

long process_spawn_placeholder(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Child: replace image with a long-lived idler. If exec fails
         * (no sleep binary), fall back to idling in-process. */
        execlp("sleep", "sleep", "3600", (char *)NULL);
        for (;;) {
            pause();
        }
        _exit(127); /* unreachable */
    }
    return (long)pid;
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

long process_spawn_exec(const process_exec_t *spec)
{
    int errpipe[2];
    pid_t pid;
    int flags;

    if (spec == NULL || spec->path == NULL || spec->path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (pipe(errpipe) != 0)
        return -1;

    /* Close the write end automatically on a successful exec, so the parent's
     * read() returns EOF exactly when execv() succeeded. On exec failure the
     * child writes its errno and the parent's read() returns data instead. */
    flags = fcntl(errpipe[1], F_GETFD);
    if (flags < 0 || fcntl(errpipe[1], F_SETFD, flags | FD_CLOEXEC) < 0) {
        int e = errno;
        (void)close(errpipe[0]);
        (void)close(errpipe[1]);
        errno = e;
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        int e = errno;
        (void)close(errpipe[0]);
        (void)close(errpipe[1]);
        errno = e;
        return -1;
    }

    if (pid == 0) {
        /* Child: report setup/exec errno through the pipe, then exit. On a
         * successful exec the pipe closes itself and this path is never
         * reached -- the child never falls through to a placeholder. */
        int e;
        (void)close(errpipe[0]);

        if (spec->cwd != NULL && chdir(spec->cwd) != 0) {
            e = errno;
            (void)write(errpipe[1], &e, sizeof(e));
            _exit(127);
        }

        if (spec->argv != NULL) {
            (void)execv(spec->path, (char *const *)spec->argv);
        } else {
            char *only[2];
            only[0] = (char *)spec->path;
            only[1] = NULL;
            (void)execv(spec->path, only);
        }

        e = errno;
        (void)write(errpipe[1], &e, sizeof(e));
        _exit(127);
    }

    /* Parent: close the write end so read() can observe EOF, then read the
     * child's errno (if any) before deciding success/failure. */
    (void)close(errpipe[1]);
    {
        int child_errno = 0;
        ssize_t got = 0;
        int read_errno = 0;

        for (;;) {
            char *dst = (char *)&child_errno + got;
            ssize_t n = read(errpipe[0], dst, sizeof(child_errno) - (size_t)got);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                read_errno = errno;
                got = -1;
                break;
            }
            if (n == 0)
                break; /* EOF: exec succeeded */
            got += n;
            if ((size_t)got == sizeof(child_errno))
                break;
        }
        (void)close(errpipe[0]);

        if (got > 0) {
            /* execv/chdir failed in the child: reap it, surface its errno. */
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            errno = child_errno;
            return -1;
        }
        if (got < 0) {
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            errno = read_errno;
            return -1;
        }
    }

    return (long)pid;
}

int process_wait(long pid, process_status_t *out)
{
    int status;
    pid_t w;

    if (pid <= 0)
        return -1;

    for (;;) {
        w = waitpid((pid_t)pid, &status, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1; /* ECHILD when the child was already reaped */
        }
        if (w == (pid_t)pid)
            break;
    }

    if (out != NULL) {
        if (WIFEXITED(status)) {
            out->exited = 1;
            out->exit_code = WEXITSTATUS(status);
            out->term_signal = 0;
        } else if (WIFSIGNALED(status)) {
            out->exited = 0;
            out->exit_code = 0;
            out->term_signal = WTERMSIG(status);
        } else {
            out->exited = 0;
            out->exit_code = 0;
            out->term_signal = 0;
        }
    }
    return 0;
}

/* Everything the child needs to run the gate protocol. Lives on the parent's
 * stack; the child (fork or clone without CLONE_VM) receives a copy-on-write
 * copy, and only reads it between the release signal and exec. */
struct gated_child_ctx {
    const process_exec_t *spec;
    const process_gate_config_t *cfg;
    int rel0, rel1, status0, status1;
};

/* Child-side gate protocol. Never returns. */
static void gated_child_run(struct gated_child_ctx *c)
{
    char b;
    ssize_t n;
    int e;

    (void)close(c->rel1);
    (void)close(c->status0);

    /* Block until the parent releases (or closes the channel to abort). */
    do {
        n = read(c->rel0, &b, 1);
    } while (n < 0 && errno == EINTR);
    (void)close(c->rel0);
    if (n <= 0)
        _exit(127); /* aborted: never exec */

    /* Child-side setup (e.g. namespace configuration) before the workload. */
    if (c->cfg != NULL && c->cfg->child_prepare != NULL) {
        e = c->cfg->child_prepare(c->cfg->child_arg);
        if (e != 0) {
            (void)write(c->status1, &e, sizeof(e));
            _exit(127);
        }
    }

    if (c->spec->cwd != NULL && chdir(c->spec->cwd) != 0) {
        e = errno;
        (void)write(c->status1, &e, sizeof(e));
        _exit(127);
    }

    if (c->spec->argv != NULL) {
        (void)execvp(c->spec->path, (char *const *)c->spec->argv);
    } else {
        char *only[2];
        only[0] = (char *)c->spec->path;
        only[1] = NULL;
        (void)execvp(c->spec->path, only);
    }
    e = errno;
    (void)write(c->status1, &e, sizeof(e));
    _exit(127);
}

/* clone(2) entry point: runs on the freshly allocated gate stack. */
static int gated_clone_entry(void *arg)
{
    gated_child_run((struct gated_child_ctx *)arg);
    return 0; /* unreachable */
}

static void gate_free_stack(process_gate_t *gate)
{
    if (gate->stack != NULL && gate->stack_size != 0)
        (void)munmap(gate->stack, gate->stack_size);
    gate->stack = NULL;
    gate->stack_size = 0;
}

long process_spawn_gated(const process_exec_t *spec,
                         const process_gate_config_t *cfg,
                         process_gate_t *gate)
{
    int status_pipe[2];
    int rel[2];
    pid_t pid;
    int fdflags;
    int nflags;
    struct gated_child_ctx ctx;
    void *stack = NULL;
    size_t stack_size = 0;

    if (gate == NULL) {
        errno = EINVAL;
        return -1;
    }
    gate->pid = -1;
    gate->release_fd = -1;
    gate->status_fd = -1;
    gate->stack = NULL;
    gate->stack_size = 0;

    if (spec == NULL || spec->path == NULL || spec->path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    nflags = (cfg != NULL) ? cfg->clone_flags : 0;

    /* Release channel (socketpair so the parent can send with MSG_NOSIGNAL)
     * plus a CLOEXEC status pipe that reports a pre-exec failure. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, rel) != 0)
        return -1;

    if (pipe(status_pipe) != 0) {
        int e = errno;
        (void)close(rel[0]);
        (void)close(rel[1]);
        errno = e;
        return -1;
    }

    fdflags = fcntl(status_pipe[1], F_GETFD);
    if (fdflags < 0 || fcntl(status_pipe[1], F_SETFD, fdflags | FD_CLOEXEC) < 0) {
        int e = errno;
        (void)close(rel[0]);
        (void)close(rel[1]);
        (void)close(status_pipe[0]);
        (void)close(status_pipe[1]);
        errno = e;
        return -1;
    }

    /* A namespace child needs its own stack for clone(2). */
    if (nflags != 0) {
        stack_size = NEXUS_GATE_STACK;
        stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (stack == MAP_FAILED) {
            int e = errno;
            (void)close(rel[0]);
            (void)close(rel[1]);
            (void)close(status_pipe[0]);
            (void)close(status_pipe[1]);
            errno = e;
            return -1;
        }
    }

    ctx.spec = spec;
    ctx.cfg = cfg;
    ctx.rel0 = rel[0];
    ctx.rel1 = rel[1];
    ctx.status0 = status_pipe[0];
    ctx.status1 = status_pipe[1];

    if (nflags != 0) {
        /* Born directly inside the requested namespaces (incl. CLONE_NEWPID,
         * where the child is PID 1). Requires CAP_SYS_ADMIN in the current user
         * namespace; unprivileged callers get EPERM here and no child is made. */
        pid = (pid_t)clone(gated_clone_entry, (char *)stack + stack_size,
                           nflags | SIGCHLD, &ctx);
    } else {
        pid = fork();
        if (pid == 0) {
            gated_child_run(&ctx);
            _exit(127); /* unreachable */
        }
    }

    if (pid < 0) {
        int e = errno;
        if (stack != NULL)
            (void)munmap(stack, stack_size);
        (void)close(rel[0]);
        (void)close(rel[1]);
        (void)close(status_pipe[0]);
        (void)close(status_pipe[1]);
        errno = e;
        return -1;
    }

    /* Parent: keep the release write end and the status read end. */
    (void)close(rel[0]);
    (void)close(status_pipe[1]);
    gate->pid = (long)pid;
    gate->release_fd = rel[1];
    gate->status_fd = status_pipe[0];
    gate->stack = stack;
    gate->stack_size = stack_size;
    return (long)pid;
}

long process_gate_release(process_gate_t *gate)
{
    int child_errno = 0;
    ssize_t got = 0;
    int read_errno = 0;
    char b = 1;
    long pid;

    if (gate == NULL || gate->pid <= 0 || gate->release_fd < 0 ||
        gate->status_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    pid = gate->pid;

    /* Unblock the child. MSG_NOSIGNAL: if it already died, report failure
     * instead of taking SIGPIPE. */
    for (;;) {
        if (send(gate->release_fd, &b, 1, MSG_NOSIGNAL) >= 0)
            break;
        if (errno == EINTR)
            continue;
        {
            int e = errno;
            int st;
            (void)close(gate->release_fd);
            gate->release_fd = -1;
            (void)close(gate->status_fd);
            gate->status_fd = -1;
            gate->pid = -1;
            while (waitpid((pid_t)pid, &st, 0) < 0 && errno == EINTR) {
            }
            gate_free_stack(gate);
            errno = e;
            return -1;
        }
    }
    (void)close(gate->release_fd);
    gate->release_fd = -1;

    /* Exec succeeded exactly when the CLOEXEC write end closes -> read() hits
     * EOF. A pre-exec chdir/exec failure instead delivers the child's errno. */
    for (;;) {
        char *dst = (char *)&child_errno + got;
        ssize_t n = read(gate->status_fd, dst,
                         sizeof(child_errno) - (size_t)got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            read_errno = errno;
            got = -1;
            break;
        }
        if (n == 0)
            break; /* EOF: exec succeeded */
        got += n;
        if ((size_t)got == sizeof(child_errno))
            break;
    }
    (void)close(gate->status_fd);
    gate->status_fd = -1;
    gate->pid = -1;

    if (got > 0) {
        int st;
        while (waitpid((pid_t)pid, &st, 0) < 0 && errno == EINTR) {
        }
        gate_free_stack(gate);
        errno = child_errno;
        return -1;
    }
    if (got < 0) {
        int st;
        while (waitpid((pid_t)pid, &st, 0) < 0 && errno == EINTR) {
        }
        gate_free_stack(gate);
        errno = read_errno;
        return -1;
    }
    gate_free_stack(gate);
    return pid;
}

void process_gate_abort(process_gate_t *gate)
{
    if (gate == NULL)
        return;

    if (gate->pid > 0) {
        int st;
        (void)kill((pid_t)gate->pid, SIGKILL);
        while (waitpid((pid_t)gate->pid, &st, 0) < 0 && errno == EINTR) {
        }
        gate->pid = -1;
    }
    if (gate->release_fd >= 0) {
        (void)close(gate->release_fd);
        gate->release_fd = -1;
    }
    if (gate->status_fd >= 0) {
        (void)close(gate->status_fd);
        gate->status_fd = -1;
    }
    gate_free_stack(gate);
}

#endif
