/*
 * NexusDB — Container Engine demonstration (readable walkthrough).
 *
 * This program drives the real Container Engine public API declared in
 * include/container/container.h. It does NOT reimplement any lifecycle
 * logic: every state transition and error is produced by calling
 * container_create / container_start / container_stop / container_delete /
 * container_status / container_get and observing the actual results.
 *
 * WHAT THIS DEMONSTRATES (all backed by real return values + observed state):
 *   - configuration validation (bad names, bad CPU limit, over-long path)
 *   - creation + initial CREATED state
 *   - starting the container and verifying the RUNNING state
 *   - rejecting a duplicate start (state machine)
 *   - stopping the container and verifying the STOPPED state
 *   - deleting the container and verifying the DELETED tombstone
 *   - error paths: unknown names, illegal states, bad status arguments
 *
 * WHAT THIS DEMONSTRATES, EXPLICITLY, ARE *NOT* IMPLEMENTED:
 *   - The runtime launches a PLACEHOLDER child process (src/container/process.c:
 *     "ping" on Windows, "sleep" on Linux). It does NOT launch the database
 *     engine. The db_engine field is only a recorded path string.
 *   - Linux namespaces and cgroups v2 are NOT implemented. cgroup_path is only
 *     a recorded string; nothing is created in /sys/fs/cgroup.
 *   - Resource limits (memory_bytes / cpu_percent) are NOT enforced by the OS.
 *
 * Exit code: 0 if every check passed, 1 if any check failed.
 * Total checks are counted dynamically at run time.
 *
 * Portability: builds as C11 on the Windows dev shim and on Linux/WSL2.
 */
#include <stdio.h>
#include <string.h>

#include "container/container.h"

#ifdef _WIN32
#include <process.h>
#define NEXUS_GETPID() ((long)_getpid())
#else
#include <unistd.h>
#define NEXUS_GETPID() ((long)getpid())
#endif

/* ---- dynamic check accounting ---- */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(desc, expr) do { \
    if (expr) { g_pass++; printf("  PASS: %s\n", (desc)); } \
    else      { g_fail++; printf("  FAIL: %s\n", (desc)); } \
} while (0)

/* ---- reliable cleanup: never leave a placeholder child running ---- */

#define MAX_TRACKED 4
static const char *g_tracked[MAX_TRACKED];
static int g_tracked_count = 0;

static void track(const char *name)
{
    if (name != NULL && g_tracked_count < MAX_TRACKED)
        g_tracked[g_tracked_count++] = name;
}

/* Stop (if running) and delete (if live) every tracked container. Called on
 * every exit path so a failed check can never leak a background process and
 * never leave a non-DELETED slot behind. Tolerant of names that were never
 * created (container_get returns NULL). */
static void cleanup_all(void)
{
    int i;
    for (i = 0; i < g_tracked_count; i++) {
        const nexus_container_t *c = container_get(g_tracked[i]);
        if (c == NULL)
            continue;
        if (c->state == NEXUS_RUNNING)
            (void)container_stop(g_tracked[i]);
        c = container_get(g_tracked[i]);
        if (c != NULL && c->state != NEXUS_DELETED)
            (void)container_delete(g_tracked[i]);
    }
}

int main(void)
{
    char name[NEXUS_NAME_MAX];
    char ghost[NEXUS_NAME_MAX];
    char longname[128];
    char longpath[320];
    char buf[1024];
    char tiny[8];
    long pid;
    int rc;
    const nexus_container_t *c;

    /* Unique per process: the registry is in-process static storage, and a
     * PID-suffixed name cannot collide with the lifecycle test or any other
     * binary that might run concurrently. */
    snprintf(name, sizeof(name), "demo_%ld", NEXUS_GETPID());
    snprintf(ghost, sizeof(ghost), "ghost_%ld", NEXUS_GETPID());

    track(name);
    track(ghost);

    printf("=== NexusDB Container Engine demonstration ===\n");
    printf("(uses the real container API; placeholder child process only)\n\n");
    printf("Container name: %s\n\n", name);

    /* ------------------------------------------------------------------ */
    printf("[1] Configuration validation (real container_create returns)\n");

    memset(longname, 'a', 100);
    longname[100] = '\0';
    memset(longpath, 'p', 300);
    longpath[300] = '\0';

    rc = container_create(NULL, 0, 0, NULL);
    CHECK("reject NULL name", rc == NEXUS_ERR_INVALID);
    rc = container_create("", 0, 0, NULL);
    CHECK("reject empty name", rc == NEXUS_ERR_INVALID);
    rc = container_create("bad name!", 0, 0, NULL);
    CHECK("reject name with space/! (invalid chars)", rc == NEXUS_ERR_INVALID);
    rc = container_create(longname, 0, 0, NULL);
    CHECK("reject name >= 64 chars", rc == NEXUS_ERR_INVALID);
    rc = container_create(name, 0, 101, NULL);
    CHECK("reject cpu_percent > 100", rc == NEXUS_ERR_INVALID);
    rc = container_create(name, 0, 0, longpath);
    CHECK("reject db_engine path >= 256 chars", rc == NEXUS_ERR_INVALID);

    /* ------------------------------------------------------------------ */
    printf("\n[2] Create + initial state\n");

    rc = container_create(name, 256UL * 1024 * 1024, 50, "./db_engine");
    CHECK("create valid container returns NEXUS_OK", rc == NEXUS_OK);

    c = container_get(name);
    CHECK("container_get finds the container", c != NULL);
    CHECK("initial state is CREATED", c != NULL && c->state == NEXUS_CREATED);
    CHECK("no child yet (pid == -1)", c != NULL && c->pid == -1);
    CHECK("container id was assigned", c != NULL && c->id[0] != '\0');
    CHECK("memory limit recorded (256 MiB)",
          c != NULL && c->memory_bytes == 256UL * 1024 * 1024);
    CHECK("cpu limit recorded (50%)", c != NULL && c->cpu_percent == 50);
    CHECK("db_engine path recorded (string only)",
          c != NULL && strcmp(c->db_engine, "./db_engine") == 0);
    CHECK("cgroup path recorded (string only, not created)",
          c != NULL && strstr(c->cgroup_path, "/sys/fs/cgroup/nexusdb/") != NULL);

    rc = container_create(name, 0, 0, NULL);
    CHECK("duplicate create of live container -> EXISTS",
          rc == NEXUS_ERR_EXISTS);

    rc = container_status(name, buf, sizeof(buf));
    CHECK("status works while CREATED", rc == NEXUS_OK);
    CHECK("status shows CREATED", strstr(buf, "CREATED") != NULL);
    printf("--- status (CREATED) ---\n%s------------------------\n", buf);

    /* ------------------------------------------------------------------ */
    printf("\n[3] Start + running state\n");

    rc = container_start(name);
    CHECK("start returns NEXUS_OK", rc == NEXUS_OK);

    c = container_get(name);
    CHECK("state is RUNNING after start",
          c != NULL && c->state == NEXUS_RUNNING);
    CHECK("a live child pid was assigned", c != NULL && c->pid > 0);
    pid = (c != NULL) ? c->pid : -1;

    rc = container_start(name);
    CHECK("duplicate start rejected with STATE", rc == NEXUS_ERR_STATE);

    rc = container_status(name, buf, sizeof(buf));
    CHECK("status works while RUNNING", rc == NEXUS_OK);
    CHECK("status shows RUNNING", strstr(buf, "RUNNING") != NULL);
    printf("--- status (RUNNING, pid=%ld) ---\n%s------------------------\n",
           pid, buf);

    /* ------------------------------------------------------------------ */
    printf("\n[4] Delete-while-running guard (no orphaned children)\n");

    rc = container_delete(name);
    CHECK("delete while RUNNING rejected with STATE", rc == NEXUS_ERR_STATE);

    /* ------------------------------------------------------------------ */
    printf("\n[5] Stop + stopped state\n");

    rc = container_stop(name);
    CHECK("stop returns NEXUS_OK", rc == NEXUS_OK);

    c = container_get(name);
    CHECK("state is STOPPED after stop",
          c != NULL && c->state == NEXUS_STOPPED);
    CHECK("child pid cleared after stop (reaped)", c != NULL && c->pid == -1);

    rc = container_stop(name);
    CHECK("duplicate stop rejected with STATE", rc == NEXUS_ERR_STATE);

    /* ------------------------------------------------------------------ */
    printf("\n[6] Restart from STOPPED\n");

    rc = container_start(name);
    CHECK("restart from STOPPED returns NEXUS_OK", rc == NEXUS_OK);
    c = container_get(name);
    CHECK("state is RUNNING after restart",
          c != NULL && c->state == NEXUS_RUNNING && c->pid > 0);

    rc = container_stop(name);
    CHECK("stop restart returns NEXUS_OK", rc == NEXUS_OK);

    /* ------------------------------------------------------------------ */
    printf("\n[7] Delete + deletion behavior\n");

    rc = container_delete(name);
    CHECK("delete returns NEXUS_OK", rc == NEXUS_OK);

    c = container_get(name);
    CHECK("tombstone remains and state is DELETED",
          c != NULL && c->state == NEXUS_DELETED);

    rc = container_status(name, buf, sizeof(buf));
    CHECK("status works for the DELETED tombstone", rc == NEXUS_OK);
    CHECK("status shows DELETED", strstr(buf, "DELETED") != NULL);
    printf("--- status (DELETED) ---\n%s------------------------\n", buf);

    rc = container_delete(name);
    CHECK("second delete rejected with STATE", rc == NEXUS_ERR_STATE);

    rc = container_start(name);
    CHECK("start on DELETED rejected with STATE", rc == NEXUS_ERR_STATE);

    /* ------------------------------------------------------------------ */
    printf("\n[8] Re-create over a tombstone (slot reuse)\n");

    rc = container_create(name, 64UL * 1024 * 1024, 10, NULL);
    CHECK("re-create after delete returns NEXUS_OK", rc == NEXUS_OK);
    c = container_get(name);
    CHECK("re-created container is CREATED with no pid",
          c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
    CHECK("re-created container cleared db_engine",
          c != NULL && c->db_engine[0] == '\0');

    /* ------------------------------------------------------------------ */
    printf("\n[9] Unknown names and bad status arguments\n");

    CHECK("get(unknown) returns NULL", container_get(ghost) == NULL);
    CHECK("start(unknown) -> NOT_FOUND",
          container_start(ghost) == NEXUS_ERR_NOT_FOUND);
    CHECK("stop(unknown) -> NOT_FOUND",
          container_stop(ghost) == NEXUS_ERR_NOT_FOUND);
    CHECK("delete(unknown) -> NOT_FOUND",
          container_delete(ghost) == NEXUS_ERR_NOT_FOUND);
    CHECK("status(unknown) -> NOT_FOUND",
          container_status(ghost, buf, sizeof(buf)) == NEXUS_ERR_NOT_FOUND);

    CHECK("status with NULL buffer -> INVALID",
          container_status(name, NULL, sizeof(buf)) == NEXUS_ERR_INVALID);
    CHECK("status with zero length -> INVALID",
          container_status(name, buf, 0) == NEXUS_ERR_INVALID);
    CHECK("status with too-small buffer -> INVALID",
          container_status(name, tiny, sizeof(tiny)) == NEXUS_ERR_INVALID);

    /* ------------------------------------------------------------------ */
    printf("\n[10] Helper strings\n");

    CHECK("state_str(CREATED) is CREATED",
          strcmp(container_state_str(NEXUS_CREATED), "CREATED") == 0);
    CHECK("state_str(DELETED) is DELETED",
          strcmp(container_state_str(NEXUS_DELETED), "DELETED") == 0);
    CHECK("strerror(EXISTS) is non-empty",
          container_strerror(NEXUS_ERR_EXISTS) != NULL &&
          container_strerror(NEXUS_ERR_EXISTS)[0] != '\0');

    /* ------------------------------------------------------------------ */
    cleanup_all();

    printf("\n=== %s: %d/%d checks passed, %d failed ===\n",
           g_fail == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
           g_pass, g_pass + g_fail, g_fail);

    return g_fail == 0 ? 0 : 1;
}
