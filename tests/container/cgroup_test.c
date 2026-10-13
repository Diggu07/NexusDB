/*
 * NexusDB — cgroup reliability test (Part 1).
 *
 * Uses an isolated root via cgroup_init(root_override) so it never touches
 * /sys/fs/cgroup/nexusdb and needs no sudo. Exercises: init idempotency,
 * invalid args, create/set_limits/read_metrics roundtrip, unlimited ("max"),
 * missing-file honesty (PROCESS, not fake zero pressure), attach validation,
 * non-empty destroy refusal (STATE), and idempotent destroy.
 *
 * Exit 0 on success, 1 on any failure. Same PASS/FAIL style as
 * tests/container/container_test.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid_compat() _getpid()
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define getpid_compat() (int)getpid()
#endif

#include "container/cgroup.h"
#include "container/container.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

/* Build an isolated, per-run root under the OS temp dir. */
static void test_root(char *buf, size_t len)
{
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
#ifdef _WIN32
    if (!tmp) tmp = ".";
    snprintf(buf, len, "%s\\nexus_cgtest_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#else
    if (!tmp) tmp = "/tmp";
    snprintf(buf, len, "%s/nexus_cgtest_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#endif
}

static void child_path(char *buf, size_t len, const char *dir, const char *file)
{
#ifdef _WIN32
    snprintf(buf, len, "%s\\%s", dir, file);
#else
    snprintf(buf, len, "%s/%s", dir, file);
#endif
}

int main(void)
{
    char root[NEXUS_PATH_MAX];
    char path[NEXUS_PATH_MAX];
    char fp[NEXUS_PATH_MAX + 32];
    int rc;
    nexus_cgroup_metrics_t m;

    printf("=== NexusDB cgroup reliability test (isolated root) ===\n\n");

    test_root(root, sizeof(root));
    printf("isolated root: %s\n", root);

    /* 1. Init is idempotent and selects the override root. */
    rc = cgroup_init(root);
    CHECK("cgroup_init isolated root", rc == NEXUS_OK);
    rc = cgroup_init(root);
    CHECK("cgroup_init idempotent", rc == NEXUS_OK);
    rc = cgroup_init("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK("cgroup_init rejects overlong override", rc == NEXUS_ERR_INVALID);

    /* 2. Invalid args. */
    rc = cgroup_create(NULL, path, sizeof(path));
    CHECK("cgroup_create NULL name -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_create("", path, sizeof(path));
    CHECK("cgroup_create empty name -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_create("a/b", path, sizeof(path));
    CHECK("cgroup_create separator -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_create("..", path, sizeof(path));
    CHECK("cgroup_create dotdot -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_create("ok", NULL, 0);
    CHECK("cgroup_create NULL out -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_set_limits(NULL, 1024, 50);
    CHECK("set_limits NULL path -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_set_limits(root, 1024, 101);
    CHECK("set_limits cpu > 100 -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_attach(NULL, 123);
    CHECK("attach NULL path -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_attach(root, 0);
    CHECK("attach pid 0 -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_read_metrics(NULL, &m);
    CHECK("metrics NULL path -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_read_metrics(root, NULL);
    CHECK("metrics NULL out -> INVALID", rc == NEXUS_ERR_INVALID);
    rc = cgroup_destroy(NULL);
    CHECK("destroy NULL -> INVALID", rc == NEXUS_ERR_INVALID);

    /* 3. Roundtrip: create -> limits -> metrics. */
    rc = cgroup_create("cg1", path, sizeof(path));
    CHECK("cgroup_create cg1", rc == NEXUS_OK);
    rc = cgroup_set_limits(path, 256UL * 1024 * 1024, 50);
    CHECK("set_limits 256MB/50", rc == NEXUS_OK);
    rc = cgroup_read_metrics(path, &m);
    CHECK("read_metrics cg1 OK", rc == NEXUS_OK);
    CHECK("memory_max roundtrips", m.memory_max == 256UL * 1024 * 1024);
    {
        double want = m.memory_max > 0
            ? (double)m.memory_current / (double)m.memory_max : 0.0;
        double diff = m.memory_pressure - want;
        if (diff < 0) diff = -diff;
        CHECK("pressure == current/max", diff < 1e-9);
    }
    printf("--- cg1: max=%lu current=%lu pressure=%.4f cpu=%lu ---\n",
           m.memory_max, m.memory_current, m.memory_pressure, m.cpu_usage_usec);

    /* 4. Unlimited ("max") is valid with pressure 0. */
    rc = cgroup_set_limits(path, 0, 0);
    CHECK("set_limits unlimited", rc == NEXUS_OK);
    rc = cgroup_read_metrics(path, &m);
    CHECK("metrics unlimited OK", rc == NEXUS_OK && m.memory_max == 0 && m.memory_pressure == 0.0);

    /* 5. Honesty: missing memory.current / memory.max must fail, not fake 0. */
    rc = cgroup_create("cg2", path, sizeof(path));
    CHECK("cgroup_create cg2", rc == NEXUS_OK);
    rc = cgroup_set_limits(path, 1024, 10);
    CHECK("cg2 limits", rc == NEXUS_OK);
    child_path(fp, sizeof(fp), path, "memory.current");
    (void)remove(fp);
    rc = cgroup_read_metrics(path, &m);
    CHECK("missing memory.current -> PROCESS (no fake zero)", rc == NEXUS_ERR_PROCESS);
    rc = cgroup_set_limits(path, 2048, 10); /* restore max; current still missing */
    (void)rc;
    /* Recreate current with valid content, then drop max instead. */
    {
        FILE *f = fopen(fp, "w");
        if (f) { fprintf(f, "512\n"); fclose(f); }
    }
    child_path(fp, sizeof(fp), path, "memory.max");
    (void)remove(fp);
    rc = cgroup_read_metrics(path, &m);
    CHECK("missing memory.max -> PROCESS (no fake zero)", rc == NEXUS_ERR_PROCESS);
    /* Corrupt content also fails. */
    {
        FILE *f = fopen(fp, "w");
        if (f) { fprintf(f, "not-a-number\n"); fclose(f); }
    }
    /* memory.current still valid from above; max corrupt -> PROCESS */
    child_path(fp, sizeof(fp), path, "memory.current");
    {
        FILE *f = fopen(fp, "w");
        if (f) { fprintf(f, "512\n"); fclose(f); }
    }
    child_path(fp, sizeof(fp), path, "memory.max");
    {
        FILE *f = fopen(fp, "w");
        if (f) { fprintf(f, "not-a-number\n"); fclose(f); }
    }
    rc = cgroup_read_metrics(path, &m);
    CHECK("corrupt memory.max -> PROCESS", rc == NEXUS_ERR_PROCESS);
    /* Cleanup cg2 (files may be half-removed; destroy must still succeed). */
    {
        char tmp[NEXUS_PATH_MAX + 32];
        child_path(tmp, sizeof(tmp), path, "cgroup.procs");
        (void)remove(tmp);
        child_path(tmp, sizeof(tmp), path, "memory.current");
        (void)remove(tmp);
        child_path(tmp, sizeof(tmp), path, "memory.max");
        (void)remove(tmp);
        /* Re-mkdir is idempotent; then destroy empty. */
        char cg2[NEXUS_PATH_MAX];
        rc = cgroup_create("cg2", cg2, sizeof(cg2));
        CHECK("cg2 re-create idempotent", rc == NEXUS_OK);
        child_path(tmp, sizeof(tmp), cg2, "cgroup.procs");
        (void)remove(tmp); /* ensure empty for destroy */
        rc = cgroup_destroy(cg2);
        CHECK("cg2 destroy after cleanup", rc == NEXUS_OK);
    }

    /* 6. Live procs block destroy; stale dead pids drain. */
    rc = cgroup_create("cg3", path, sizeof(path));
    CHECK("cgroup_create cg3", rc == NEXUS_OK);
    rc = cgroup_set_limits(path, 4096, 10);
    CHECK("cg3 limits", rc == NEXUS_OK);
    rc = cgroup_attach(path, (long)getpid_compat()); /* live: self */
    CHECK("attach live pid writes procs", rc == NEXUS_OK);
    rc = cgroup_destroy(path);
    CHECK("destroy live -> STATE", rc == NEXUS_ERR_STATE);
    child_path(fp, sizeof(fp), path, "cgroup.procs");
    (void)remove(fp); /* simulate drained cgroup (shim) */
    rc = cgroup_destroy(path);
    CHECK("destroy drained -> OK", rc == NEXUS_OK);
    rc = cgroup_destroy(path);
    CHECK("destroy idempotent -> OK", rc == NEXUS_OK);
    /* Stale dead pid must not block destroy (stop/crash recovery). */
    rc = cgroup_create("cg3stale", path, sizeof(path));
    CHECK("cgroup_create cg3stale", rc == NEXUS_OK);
    rc = cgroup_attach(path, 2147483647L); /* almost certainly dead */
    CHECK("attach dead pid writes procs", rc == NEXUS_OK);
    rc = cgroup_destroy(path);
    CHECK("destroy stale dead pid -> OK", rc == NEXUS_OK);

    /* 7. Attach to a missing cgroup fails (no silent success). */
    {
        char missing[NEXUS_PATH_MAX + 32];
        child_path(missing, sizeof(missing), root, "does-not-exist");
        rc = cgroup_attach(missing, 12345);
        CHECK("attach missing cgroup -> PROCESS", rc == NEXUS_ERR_PROCESS);
        rc = cgroup_set_limits(missing, 1024, 10);
        CHECK("set_limits missing cgroup -> PROCESS", rc == NEXUS_ERR_PROCESS);
    }

    /* 8. Cleanup cg1 + root. */
    {
        char cg1[NEXUS_PATH_MAX];
        rc = cgroup_create("cg1", cg1, sizeof(cg1));
        CHECK("cg1 re-create idempotent", rc == NEXUS_OK);
        child_path(fp, sizeof(fp), cg1, "cgroup.procs");
        (void)remove(fp);
        rc = cgroup_destroy(cg1);
        CHECK("cg1 destroy", rc == NEXUS_OK);
    }
#ifdef _WIN32
    (void)_rmdir(root);
#else
    (void)rmdir(root);
#endif

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
