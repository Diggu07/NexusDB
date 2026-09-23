/*
 * NexusDB — Container Engine foundation: metadata registry + lifecycle.
 * See include/container/container.h for the API contract.
 */
#include "container/container.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../runtime/process.h"

#define NEXUS_MAX_CONTAINERS 64

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
    slot_t *dup, *s;

    if (!name_valid(name))
        return NEXUS_ERR_INVALID;
    if (cpu_percent > 100)
        return NEXUS_ERR_INVALID;
    if (db_engine != NULL && strlen(db_engine) >= NEXUS_PATH_MAX)
        return NEXUS_ERR_INVALID;

    dup = find_slot(name);
    if (dup != NULL && dup->c.state != NEXUS_DELETED)
        return NEXUS_ERR_EXISTS; /* duplicate live container */

    s = (dup != NULL) ? dup : free_slot(); /* reuse tombstone if present */
    if (s == NULL)
        return NEXUS_ERR_NOMEM;

    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    snprintf(s->c.name, sizeof(s->c.name), "%s", name);
    snprintf(s->c.id, sizeof(s->c.id), "nx-%06lu", g_next_id++);
    s->c.state = NEXUS_CREATED;
    s->c.pid = -1;
    s->c.memory_bytes = memory_bytes;
    s->c.cpu_percent = cpu_percent;
    /* Intended cgroup v2 path — recorded for Rehan's Bridge, NOT created
     * until the cgroups phase. */
    snprintf(s->c.cgroup_path, sizeof(s->c.cgroup_path),
             "/sys/fs/cgroup/nexusdb/%s", name);
    if (db_engine != NULL)
        snprintf(s->c.db_engine, sizeof(s->c.db_engine), "%s", db_engine);
    else
        s->c.db_engine[0] = '\0';
    s->c.created_at = (long)time(NULL);

    return NEXUS_OK;
}

int container_start(const char *name)
{
    slot_t *s;
    long pid;

    if (!name_valid(name))
        return NEXUS_ERR_INVALID;
    s = find_slot(name);
    if (s == NULL)
        return NEXUS_ERR_NOT_FOUND;
    if (s->c.state != NEXUS_CREATED && s->c.state != NEXUS_STOPPED)
        return NEXUS_ERR_STATE; /* RUNNING or DELETED cannot start */
    if (s->c.pid != -1 && process_is_alive(s->c.pid))
        return NEXUS_ERR_STATE; /* stale bookkeeping: child still alive */

    pid = process_spawn_placeholder();
    if (pid <= 0)
        return NEXUS_ERR_PROCESS;

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
    s->c.state = NEXUS_DELETED; /* keep tombstone for status-after-delete */
    return NEXUS_OK;
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
