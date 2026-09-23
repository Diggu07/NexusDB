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

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_PROCESS_H */
