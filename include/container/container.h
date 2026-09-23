/*
 * NexusDB — Container Engine (Ayush: Container Runtime + Linux layer)
 *
 * Foundation module: container metadata + lifecycle management only.
 *
 * SCOPE (Phase 2 — runtime foundation):
 *   - In-memory container registry (name/ID, PID, state, limits, paths).
 *   - Lifecycle: create -> start -> stop -> delete.
 *   - A portable placeholder child process stands in for the DB engine
 *     until Digvijay's DB executable is ready (see src/runtime/process.*).
 *
 * EXPLICITLY OUT OF SCOPE here (later phases):
 *   - Linux namespaces (Phase 3), cgroups v2 (Phase 4), real cgroup paths
 *     are only *recorded* as strings, never created here.
 *   - OS-DB Bridge / adaptation policy (Rehan). This engine only *reports*
 *     state; it never resizes buffer pools or interprets memory pressure.
 *   - Database internals (Digvijay): SQL, pages, buffer pool, LRU, WAL.
 *
 * PORTABILITY:
 *   Real deployment target is Ubuntu/WSL2 (Linux). This dev machine has no
 *   WSL, so src/runtime/process.c has a Windows branch so the lifecycle
 *   demo compiles/runs here too. The Linux branch uses fork/exec/waitpid.
 */
#ifndef NEXUS_CONTAINER_H
#define NEXUS_CONTAINER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEXUS_NAME_MAX  64   /* incl. NUL; allowed: [A-Za-z0-9_-] */
#define NEXUS_ID_MAX    32   /* e.g. "nx-000001" */
#define NEXUS_PATH_MAX  256  /* cgroup path / db engine path */

/* Lifecycle states. DELETED is a tombstone: the slot is kept so that
 * status-after-delete is observable; creating the same name again
 * reuses the slot (re-created). */
typedef enum nexus_state {
    NEXUS_CREATED = 0,
    NEXUS_RUNNING = 1,
    NEXUS_STOPPED = 2,
    NEXUS_DELETED = 3
} nexus_state_t;

/* Result codes (0 == success, negative == error). */
typedef enum nexus_rc {
    NEXUS_OK          = 0,
    NEXUS_ERR_INVALID = -1,  /* bad name / limit / NULL arg */
    NEXUS_ERR_NOT_FOUND = -2,/* no container with that name */
    NEXUS_ERR_EXISTS  = -3,  /* live container already has that name */
    NEXUS_ERR_STATE   = -4,  /* operation illegal in current state */
    NEXUS_ERR_NOMEM   = -5,  /* registry full */
    NEXUS_ERR_PROCESS = -6   /* child spawn/stop/wait failed */
} nexus_rc_t;

/*
 * Container metadata. Owned by the registry in src/container/container.c;
 * callers must treat pointers from container_get() as read-only and must
 * NOT free them. pid == -1 means "no live child process".
 *
 * Limits: memory_bytes == 0 means "no limit configured yet";
 * cpu_percent == 0 means "no limit configured yet", else 1..100.
 * cgroup_path is the *intended* v2 path (e.g. /sys/fs/cgroup/nexusdb/<name>);
 * it is NOT created until the cgroups phase.
 */
typedef struct nexus_container {
    char name[NEXUS_NAME_MAX];
    char id[NEXUS_ID_MAX];
    nexus_state_t state;
    long pid;                          /* child PID, -1 if none */
    unsigned long memory_bytes;        /* 0 = unlimited/unset */
    unsigned int cpu_percent;          /* 0 = unset, else 1..100 */
    char cgroup_path[NEXUS_PATH_MAX];  /* recorded only, not created */
    char db_engine[NEXUS_PATH_MAX];    /* configured executable, may be "" */
    long created_at;                   /* time() at create, 0 if unknown */
} nexus_container_t;

/* Create a container record in CREATED state. db_engine may be NULL/"".
 * Fails with EXISTS if a live (non-DELETED) container has that name. */
int container_create(const char *name,
                     unsigned long memory_bytes,
                     unsigned int cpu_percent,
                     const char *db_engine);

/* Start the container: spawns the placeholder child (later: the DB engine
 * inside namespaces+cgroup) and moves CREATED/STOPPED -> RUNNING. */
int container_start(const char *name);

/* Stop a RUNNING container: SIGTERM, grace period, SIGKILL, reap
 * (no zombies), moves RUNNING -> STOPPED. */
int container_stop(const char *name);

/* Delete a container. Must be CREATED/STOPPED/DELETED; RUNNING must be
 * stopped first (returns STATE). Keeps a DELETED tombstone. */
int container_delete(const char *name);

/* Render one-line-per-field human-readable status into buf (always NUL
 * terminated). Works for DELETED tombstones too. */
int container_status(const char *name, char *buf, size_t buflen);

/* Read-only lookup; NULL if the name was never created. */
const nexus_container_t *container_get(const char *name);

/* Helpers. */
const char *container_state_str(nexus_state_t state);
const char *container_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_CONTAINER_H */
