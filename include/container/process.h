/*
 * NexusDB — child-process launcher (Ayush: Container Runtime).
 *
 * process_spawn() launches a configured executable directly (never via a
 * shell) with: namespace isolation applied in the child, cgroup membership
 * joined by the child itself BEFORE exec (no parent/child attach race), and
 * a close-on-exec error pipe so the parent can distinguish setup/exec
 * failures from a running child. A short survival check rejects exes that
 * exit immediately: this API is for long-lived daemons (DB engine), not
 * one-shot commands.
 *
 * PID-namespace semantics: unshare(CLONE_NEWPID) takes effect only for the
 * caller's FUTURE CHILDREN, not the caller itself. The spawned child is
 * therefore NOT pid-isolated itself; only its own children would be. Mount,
 * network, UTS and IPC isolation take effect in the child immediately.
 * A double-fork/clone-based PID-1 setup is deferred to a later phase.
 *
 * Linux (real path): fork() + unshare + self-attach + execv(p).
 * Windows (dev-machine shim only): CreateProcessA + mock cgroup.procs write.
 * Namespaces are ignored on Windows; cgroups still require Ubuntu/WSL2.
 *
 * Reaping: process_stop() always reaps the child (no zombies on Linux,
 * no leaked HANDLEs on Windows).
 */
#ifndef NEXUS_PROCESS_H
#define NEXUS_PROCESS_H

#include "container/namespace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn exe_path with argv (argv[0] should be the program name, array must
 * be NULL-terminated). cgroup_path may be NULL/"" (no cgroup join);
 * ns_cfg may be NULL (no namespace isolation).
 *
 * Returns child pid (>0) when exec succeeded and the child is still alive,
 * or -1 on failure (errno set: EINVAL on bad exe/argv, ETIMEDOUT on setup
 * stall, ECHILD when the child exited immediately, otherwise the child's
 * setup errno). Never returns into the child: setup failures _exit() there.
 * No shell is involved: exe_path is executed directly (PATH search only when
 * it contains no '/'). */
long process_spawn(const char *exe_path, char *const argv[],
                   const char *cgroup_path,
                   const nexus_namespace_config_t *ns_cfg);

/* Spawn the built-in placeholder idler (~1h sleep/ping) with no isolation.
 * Stand-in used when a container has no db_engine configured. */
long process_spawn_placeholder(void);

/* Terminate + reap pid. 0 on success, -1 on failure (errno/GetLastError
 * preserved for the caller to log). Idempotent-ish: an already-dead but
 * not-yet-reaped child is reaped and reported as success. */
int process_stop(long pid);

/* 1 if pid names a live (or zombie, Linux) child, 0 otherwise. */
int process_is_alive(long pid);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_PROCESS_H */
