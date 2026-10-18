/*
 * NexusDB — Container Engine foundation: metadata registry + lifecycle.
 * See include/container/container.h for the API contract.
 */
#include "container/container.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "container/cgroup.h"
#include "container/namespace.h"
#include "container/process.h"

#define NEXUS_MAX_CONTAINERS 64

/* Base directory for per-container DB-engine data. Each configured container
 * gets <root>/<name> passed to its engine as `--data-dir`. Overridable with
 * $NEXUSDB_DATA_ROOT (relative to the workload's inherited cwd). */
#define NEXUS_DATA_ROOT_ENV     "NEXUSDB_DATA_ROOT"
#define NEXUS_DATA_ROOT_DEFAULT "nexusdb_data"

typedef struct slot {
    int in_use;              /* 0 = never created, 1 = live or tombstone */
    nexus_container_t c;
} slot_t;

static slot_t g_slots[NEXUS_MAX_CONTAINERS];
static unsigned long g_next_id = 1;

/* ---- validation helpers ---- */

static int name_valid(const char *name)
{
    size_t i, n;
    if (name == NULL)
        return 0;
    n = strlen(name);
    if (n == 0 || n >= NEXUS_NAME_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!isalnum(ch) && ch != '_' && ch != '-')
            return 0;
    }
    return 1;
}

static slot_t *find_slot(const char *name)
{
    int i;
    if (name == NULL)
        return NULL;
    for (i = 0; i < NEXUS_MAX_CONTAINERS; i++) {
        if (g_slots[i].in_use && strcmp(g_slots[i].c.name, name) == 0)
            return &g_slots[i];
    }
    return NULL;
}

static slot_t *free_slot(void)
{
    int i;
    for (i = 0; i < NEXUS_MAX_CONTAINERS; i++) {
        if (!g_slots[i].in_use)
            return &g_slots[i];
    }
    return NULL;
}

/* ---- public helpers ---- */

const char *container_state_str(nexus_state_t state)
{
    switch (state) {
    case NEXUS_CREATED: return "CREATED";
    case NEXUS_RUNNING: return "RUNNING";
    case NEXUS_STOPPED: return "STOPPED";
    case NEXUS_DELETED: return "DELETED";
    default:            return "UNKNOWN";
    }
}

const char *container_strerror(int rc)
{
    switch (rc) {
    case NEXUS_OK:          return "ok";
    case NEXUS_ERR_INVALID: return "invalid argument";
    case NEXUS_ERR_NOT_FOUND: return "container not found";
    case NEXUS_ERR_EXISTS:  return "container already exists";
    case NEXUS_ERR_STATE:   return "illegal state for this operation";
    case NEXUS_ERR_NOMEM:   return "container registry full";
    case NEXUS_ERR_PROCESS: return "child process operation failed";
    case NEXUS_ERR_UNSUPPORTED: return "not supported on this platform";
    default:                return "unknown error";
    }
}

const nexus_container_t *container_get(const char *name)
{
    slot_t *s = find_slot(name);
    return s ? &s->c : NULL;
}

/* ---- lifecycle ---- */

int container_create(const char *name,
                     unsigned long memory_bytes,
                     unsigned int cpu_percent,
                     const char *db_engine)
{
    return container_create_ex(name, memory_bytes, cpu_percent, db_engine, NULL);
}

int container_create_ex(const char *name,
                        unsigned long memory_bytes,
                        unsigned int cpu_percent,
                        const char *db_engine,
                        const nexus_namespace_config_t *namespaces)
{
    slot_t *dup, *s;
    char tmp_path[NEXUS_PATH_MAX];
    int rc;

    if (!name_valid(name))
        return NEXUS_ERR_INVALID;
    if (cpu_percent > 100)
        return NEXUS_ERR_INVALID;
    if (db_engine != NULL && strlen(db_engine) >= NEXUS_PATH_MAX)
        return NEXUS_ERR_INVALID;
    if (namespaces != NULL && namespaces->hostname != NULL &&
        strlen(namespaces->hostname) >= NEXUS_NAME_MAX)
        return NEXUS_ERR_INVALID;

    dup = find_slot(name);
    if (dup != NULL && dup->c.state != NEXUS_DELETED)
        return NEXUS_ERR_EXISTS; /* duplicate live container */

    s = (dup != NULL) ? dup : free_slot(); /* reuse tombstone if present */
    if (s == NULL)
        return NEXUS_ERR_NOMEM;

    /* Provision cgroups BEFORE mutating the slot so a resource failure never
     * corrupts the registry (and never reports success for unenforced limits). */
    (void)cgroup_init(NULL); /* best-effort root setup; create() surfaces real errors */
    rc = cgroup_create(name, tmp_path, sizeof(tmp_path));
    if (rc != NEXUS_OK)
        return rc;
    rc = cgroup_set_limits(tmp_path, memory_bytes, cpu_percent);
    if (rc != NEXUS_OK) {
        (void)cgroup_destroy(tmp_path);
        return rc;
    }

    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    snprintf(s->c.name, sizeof(s->c.name), "%s", name);
    snprintf(s->c.id, sizeof(s->c.id), "nx-%06lu", g_next_id++);
    s->c.state = NEXUS_CREATED;
    s->c.pid = -1;
    s->c.memory_bytes = memory_bytes;
    s->c.cpu_percent = cpu_percent;
    snprintf(s->c.cgroup_path, sizeof(s->c.cgroup_path), "%s", tmp_path);

    if (db_engine != NULL)
        snprintf(s->c.db_engine, sizeof(s->c.db_engine), "%s", db_engine);
    else
        s->c.db_engine[0] = '\0';
    s->c.created_at = (long)time(NULL);

    /* Copy the requested namespace isolation into the record (fields were
     * zeroed above, so a NULL config leaves isolation disabled). */
    if (namespaces != NULL) {
        s->c.ns_pid = namespaces->isolate_pid ? 1 : 0;
        s->c.ns_mount = namespaces->isolate_mount ? 1 : 0;
        s->c.ns_net = namespaces->isolate_net ? 1 : 0;
        s->c.ns_uts = namespaces->isolate_uts ? 1 : 0;
        s->c.ns_ipc = namespaces->isolate_ipc ? 1 : 0;
        if (namespaces->hostname != NULL)
            snprintf(s->c.ns_hostname, sizeof(s->c.ns_hostname), "%s",
                     namespaces->hostname);
    }

    return NEXUS_OK;
}

/* 1 if the record requests any Linux namespace isolation. */
static int container_ns_requested(const nexus_container_t *c)
{
    return c->ns_pid || c->ns_mount || c->ns_net || c->ns_uts || c->ns_ipc;
}

#ifndef _WIN32
/* Rebuild a namespace config from the (verbatim, caller-owned) record. */
static void container_ns_config(const nexus_container_t *c,
                                nexus_namespace_config_t *out)
{
    out->isolate_pid = c->ns_pid;
    out->isolate_mount = c->ns_mount;
    out->isolate_net = c->ns_net;
    out->isolate_uts = c->ns_uts;
    out->isolate_ipc = c->ns_ipc;
    out->hostname = (c->ns_hostname[0] != '\0') ? c->ns_hostname : NULL;
}

/* Child-side hook: configure the namespaces the child was cloned into. Returns
 * 0 on success, else the errno to report to the parent. */
static int ns_child_prepare(void *arg)
{
    const nexus_namespace_config_t *cfg =
        (const nexus_namespace_config_t *)arg;

    if (namespace_configure_child(cfg) != NEXUS_OK)
        return errno != 0 ? errno : EPERM;
    return 0;
}
#endif

/* Build the workload exec spec for container `c`.
 *
 * - c->db_engine empty -> the built-in placeholder (sleep/ping), preserving the
 *   pre-P3 lifecycle behavior that the existing tests rely on.
 * - c->db_engine set   -> that executable, invoked as
 *   `db_engine --data-dir <root>/<name>` where <root> is $NEXUSDB_DATA_ROOT
 *   (default ./nexusdb_data).
 *
 * spec->path/argv borrow c->db_engine and the caller-owned `data_dir`/`argv`
 * buffers, which must outlive the spawn+release sequence (they live in
 * container_start's frame). Returns 0 on success, -1 when the derived path
 * would truncate or `argv` is too small. */
static int workload_spec(const nexus_container_t *c, process_exec_t *spec,
                         char *data_dir, size_t data_dir_len,
                         const char **argv, size_t argv_cap)
{
    const char *root;

    if (c->db_engine[0] == '\0') {
        static const char *const placeholder_argv[] = { "sleep", "3600", NULL };
        spec->path = "sleep";
        spec->argv = placeholder_argv;
        spec->cwd = NULL;
        return 0;
    }

    root = getenv(NEXUS_DATA_ROOT_ENV);
    if (root == NULL || root[0] == '\0')
        root = NEXUS_DATA_ROOT_DEFAULT;
    if (snprintf(data_dir, data_dir_len, "%s/%s", root, c->name) >=
        (int)data_dir_len)
        return -1;
    if (argv_cap < 4)
        return -1;

    argv[0] = c->db_engine;
    argv[1] = "--data-dir";
    argv[2] = data_dir;
    argv[3] = NULL;

    spec->path = c->db_engine;
    spec->argv = argv;
    spec->cwd = NULL;
    return 0;
}

/* Spawn the workload. On POSIX the child is spawned GATED: it is created in
 * the requested namespaces but blocked before exec, so container_start can
 * attach it to its cgroup before the workload runs. On Windows the shim has no
 * pre-exec pause, so it always launches the built-in placeholder (a configured
 * engine is rejected earlier in container_start). Returns pid (>0) or -1. */
static long workload_spawn(const process_exec_t *spec, process_gate_t *gate,
                           const process_gate_config_t *gc)
{
#ifdef _WIN32
    (void)spec;
    (void)gc;
    gate->pid = -1;
    gate->release_fd = -1;
    gate->status_fd = -1;
    gate->stack = NULL;
    gate->stack_size = 0;
    return process_spawn_placeholder();
#else
    return process_spawn_gated(spec, gc, gate);
#endif
}

/* Release a gated workload to exec (POSIX) / no-op (Windows). 0 on success,
 * -1 if the workload failed to start (already reaped). */
static int workload_release(process_gate_t *gate)
{
#ifdef _WIN32
    (void)gate;
    return 0;
#else
    return process_gate_release(gate) > 0 ? 0 : -1;
#endif
}

/* Tear down a workload that could not be attached: abort the still-blocked
 * child (POSIX) / terminate it (Windows). */
static void workload_kill(long pid, process_gate_t *gate)
{
#ifdef _WIN32
    (void)gate;
    (void)process_stop(pid);
#else
    (void)pid;
    process_gate_abort(gate);
#endif
}

int container_start(const char *name)
{
    slot_t *s;
    long pid;
    process_gate_t gate;

    if (!name_valid(name))
        return NEXUS_ERR_INVALID;
    s = find_slot(name);
    if (s == NULL)
        return NEXUS_ERR_NOT_FOUND;
    if (s->c.state != NEXUS_CREATED && s->c.state != NEXUS_STOPPED)
        return NEXUS_ERR_STATE; /* RUNNING or DELETED cannot start */
    if (s->c.pid != -1 && process_is_alive(s->c.pid))
        return NEXUS_ERR_STATE; /* stale bookkeeping: child still alive */

    gate.pid = -1;
    gate.release_fd = -1;
    gate.status_fd = -1;
    gate.stack = NULL;
    gate.stack_size = 0;

    {
        process_exec_t spec;
        char data_dir[NEXUS_PATH_MAX];
        const char *argv[4];
        const process_gate_config_t *gcp = NULL;

#ifndef _WIN32
        process_gate_config_t gc;
        nexus_namespace_config_t ns;
        if (container_ns_requested(&s->c)) {
            container_ns_config(&s->c, &ns);
            gc.clone_flags = namespace_clone_flags(&ns);
            gc.child_prepare = ns_child_prepare;
            gc.child_arg = &ns;
            gcp = &gc;
        }
#else
        /* Gated launch is POSIX-only: a configured DB engine (which needs the
         * attach-before-exec guarantee) and namespace isolation both refuse
         * rather than silently run unmanaged. */
        if (s->c.db_engine[0] != '\0' || container_ns_requested(&s->c))
            return NEXUS_ERR_UNSUPPORTED; /* explicit: no false success */
#endif

        if (workload_spec(&s->c, &spec, data_dir, sizeof(data_dir),
                          argv, sizeof(argv) / sizeof(argv[0])) != 0)
            return NEXUS_ERR_INVALID;

        /* Spawn the workload (placeholder or configured DB engine) in the
         * configured namespaces, BLOCKED before exec so it cannot run outside
         * its cgroup. */
        pid = workload_spawn(&spec, &gate, gcp);
    }
    if (pid <= 0)
        return NEXUS_ERR_PROCESS;

    /* A limit that was not applied must never be reported as success: if the
     * child cannot join its cgroup, kill it while it is still blocked and stay
     * in the prior state. */
    if (cgroup_attach(s->c.cgroup_path, pid) != NEXUS_OK) {
        workload_kill(pid, &gate);
        s->c.pid = -1;
        return NEXUS_ERR_PROCESS;
    }

    /* Attached: let the workload execute. A failed exec is reaped internally,
     * so a startup failure leaves no zombie and never reaches RUNNING. */
    if (workload_release(&gate) != 0) {
        s->c.pid = -1;
        return NEXUS_ERR_PROCESS;
    }

    s->c.pid = pid;
    s->c.state = NEXUS_RUNNING;
    return NEXUS_OK;
}

int container_stop(const char *name)
{
    slot_t *s;

    if (!name_valid(name))
        return NEXUS_ERR_INVALID;
    s = find_slot(name);
    if (s == NULL)
        return NEXUS_ERR_NOT_FOUND;
    if (s->c.state != NEXUS_RUNNING)
        return NEXUS_ERR_STATE;

    if (s->c.pid != -1) {
        if (process_stop(s->c.pid) != 0)
            return NEXUS_ERR_PROCESS;
        s->c.pid = -1;
    }
    s->c.state = NEXUS_STOPPED;
    return NEXUS_OK;
}

int container_delete(const char *name)
{
    slot_t *s;

    if (!name_valid(name))
        return NEXUS_ERR_INVALID;
    s = find_slot(name);
    if (s == NULL)
        return NEXUS_ERR_NOT_FOUND;
    if (s->c.state == NEXUS_RUNNING)
        return NEXUS_ERR_STATE; /* must stop first: no orphaned children */
    if (s->c.state == NEXUS_DELETED)
        return NEXUS_ERR_STATE; /* already deleted */

    /* Defensive: never leave a live child behind even if bookkeeping
     * drifted (e.g. pid recorded but state was CREATED). */
    if (s->c.pid != -1) {
        if (process_is_alive(s->c.pid))
            (void)process_stop(s->c.pid);
        s->c.pid = -1;
    }
    /* A failed destroy keeps the current state so the caller can retry;
     * only a removed cgroup becomes a DELETED tombstone. */
    if (cgroup_destroy(s->c.cgroup_path) != NEXUS_OK)
        return NEXUS_ERR_PROCESS;
    s->c.state = NEXUS_DELETED; /* keep tombstone for status-after-delete */
    return NEXUS_OK;
}

int container_metrics(const char *name, nexus_cgroup_metrics_t *metrics)
{
    slot_t *s;

    if (!name_valid(name) || metrics == NULL)
        return NEXUS_ERR_INVALID;
    s = find_slot(name);
    if (s == NULL)
        return NEXUS_ERR_NOT_FOUND;
    if (s->c.state == NEXUS_DELETED)
        return NEXUS_ERR_STATE;

    return cgroup_read_metrics(s->c.cgroup_path, metrics);
}

int container_status(const char *name, char *buf, size_t buflen)
{
    const slot_t *s;
    int n;

    if (!name_valid(name) || buf == NULL || buflen == 0)
        return NEXUS_ERR_INVALID;
    s = find_slot(name);
    if (s == NULL)
        return NEXUS_ERR_NOT_FOUND;

    n = snprintf(buf, buflen,
                 "Container: %s\n"
                 "ID: %s\n"
                 "State: %s\n"
                 "PID: %ld\n"
                 "Memory limit: %lu bytes\n"
                 "CPU limit: %u%%\n"
                 "Cgroup: %s\n"
                 "DB engine: %s\n",
                 s->c.name,
                 s->c.id,
                 container_state_str(s->c.state),
                 s->c.pid,
                 s->c.memory_bytes,
                 s->c.cpu_percent,
                 s->c.cgroup_path,
                 s->c.db_engine[0] ? s->c.db_engine : "(none configured)");
    if (n < 0 || (size_t)n >= buflen)
        return NEXUS_ERR_INVALID; /* caller buffer too small */
    return NEXUS_OK;
}
