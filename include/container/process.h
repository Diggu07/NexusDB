/*
 * NexusDB — placeholder child-process helper (runtime foundation).
 *
 * Linux (real path): fork() + execlp("sleep","sleep","3600") so start()
 * exercises the same fork/exec/waitpid lifecycle the future DB-engine
 * launcher will use. If exec fails, the child idles with pause() and
 * _exit()s cleanly — never returns into the caller.
 *
 * Windows (dev-machine shim only): CreateProcessA("ping -n 3600 ...").
 * Lets the lifecycle demo run on machines without WSL. NOT part of the
 * Linux design; namespaces/cgroups still require Ubuntu/WSL2.
 *
 * Reaping: process_stop() always reaps the child (no zombies on Linux,
 * no leaked HANDLEs on Windows).
 */
#ifndef NEXUS_PROCESS_H
#define NEXUS_PROCESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the placeholder child. Returns child pid (>0) or -1 on failure. */
long process_spawn_placeholder(void);

/* Terminate + reap pid. 0 on success, -1 on failure (errno/GetLastError
 * preserved for the caller to log). Idempotent-ish: an already-dead but
 * not-yet-reaped child is reaped and reported as success. */
int process_stop(long pid);

/* 1 if pid names a live (or zombie, Linux) child, 0 otherwise. */
int process_is_alive(long pid);

/* ---- direct-execution API (P0: safe primitives, not yet wired into
 *      the container lifecycle) ---- */

/* Descriptor for process_spawn_exec(). All pointers are BORROWED: they must
 * stay valid for the duration of the call only; the implementation copies no
 * string or array and takes no ownership. argv is a NULL-terminated vector;
 * when argv is NULL the child is started with { path, NULL }. No shell is
 * ever involved (direct execv). */
typedef struct process_exec {
    const char *path;        /* executable to run; required, non-empty       */
    const char *const *argv; /* NULL-terminated argv, argv[0] = program name */
    const char *cwd;         /* optional working dir; NULL = inherit current */
} process_exec_t;

/* How a reaped child ended (filled by process_wait()). */
typedef struct process_status {
    int exited;      /* 1 = normal exit (exit_code valid)
                        0 = killed by a signal (term_signal valid) */
    int exit_code;   /* normal exit code, valid iff exited != 0 */
    int term_signal; /* terminating signal, valid iff exited == 0 */
} process_status_t;

/* Launch spec->path directly (fork + execv; never a shell).
 * Returns the child pid (>0) on success, or -1 on failure with errno set:
 *   EINVAL  - NULL/empty spec or path
 *   ENOENT  - missing/unusable executable, or a bad spec->cwd
 *   ...     - the errno reported by the child's execv()/chdir()
 * On success the child has either exec'd successfully or already been reaped
 * before this returns, so a startup failure is always visible as -1 and never
 * leaves a zombie. The returned pid is compatible with process_stop() and
 * process_is_alive().
 * Platform: implemented on POSIX (Linux/WSL2). On Windows this returns -1
 * with errno == ENOSYS; use process_spawn_placeholder() there. */
long process_spawn_exec(const process_exec_t *spec);

/* Block until child `pid` terminates, reap it, and report how it ended.
 * Returns 0 on success, -1 on failure (errno preserved; ECHILD if the child
 * was already reaped, e.g. by process_stop()). out may be NULL. Do not mix
 * process_wait() and process_stop() on the same pid. */
int process_wait(long pid, process_status_t *out);

/* ---- gated execution (P1: attach-before-exec) ----
 *
 * process_spawn_gated() forks a child that is BLOCKED before it executes
 * anything, so the caller can finish setup (e.g. cgroup membership) while the
 * workload provably has not started. The caller then either releases the child
 * to exec or aborts it. This is the primitive the container lifecycle uses to
 * guarantee a workload never runs outside its resource limits, not even for an
 * instant. */

typedef struct process_gate {
    long pid;         /* blocked child pid; -1 if no child is pending */
    int  release_fd;  /* parent's end of the release channel; -1 if none */
    int  status_fd;   /* parent's end of the exec-status channel; -1 if none */
    void *stack;      /* clone(2) child stack (POSIX); NULL when unused */
    size_t stack_size;/* size of `stack`; 0 when unused */
} process_gate_t;

/* Optional spawn controls for process_spawn_gated(). A NULL config means the
 * plain P1 behavior: fork(), no namespaces, no child hook. */
typedef struct process_gate_config {
    int clone_flags;                 /* CLONE_NEW* mask; 0 => fork() (POSIX) */
    int (*child_prepare)(void *arg); /* run in child after release, before exec;
                                        return 0 = ok, else an errno to report  */
    void *child_arg;                 /* passed to child_prepare            */
} process_gate_config_t;

/* Spawn spec->path's child but keep it blocked until process_gate_release().
 * When cfg is non-NULL and cfg->clone_flags is non-zero the child is created
 * with clone(2) carrying those namespace flags, so it is born inside the new
 * namespaces; otherwise a plain fork() is used. If cfg->child_prepare is set it
 * runs in the child (inside those namespaces) after release and before exec;
 * a non-zero return is reported to the parent as that errno and the workload is
 * never executed.
 * On success fills *gate (pid > 0, both fds >= 0) and returns gate->pid. On
 * failure returns -1 with errno set and leaves *gate with pid == -1 and both
 * fds == -1. Direct execvp (never a shell); argv follows process_spawn_exec().
 * Windows: returns -1 with errno == ENOSYS; use process_spawn_placeholder(). */
long process_spawn_gated(const process_exec_t *spec,
                         const process_gate_config_t *cfg,
                         process_gate_t *gate);

/* Unblock the child so it executes, then wait until its exec has succeeded or
 * failed. On exec success returns the child pid (>0); the caller may then use
 * process_wait()/process_stop() on it. On startup (chdir/exec) failure returns
 * -1 with errno set to the child's error and reaps the child (no zombie).
 * Always closes the gate's fds and resets *gate to pid == -1. Do not call on a
 * gate whose pid is not > 0 (returns -1/EINVAL). */
long process_gate_release(process_gate_t *gate);

/* Kill the still-blocked child and reap it (no zombie), then close the gate.
 * The workload never executes. Safe (no-op) on an idle/already-reset gate or a
 * NULL pointer. */
void process_gate_abort(process_gate_t *gate);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_PROCESS_H */
