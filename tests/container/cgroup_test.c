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
#ifndef _WIN32
#define _DEFAULT_SOURCE /* symlink() for the read-back verification test */
#endif

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

/* Materialize a metric file inside a fake (temp-directory) cgroup. Real
 * kernel cgroups expose these files themselves, but on Linux cgroup_create
 * intentionally does NOT fabricate kernel files — so the isolated-root
 * fixture tests must supply controlled values themselves. */
static void write_metric(const char *dir, const char *file, const char *content)
{
    char p[NEXUS_PATH_MAX + 32];
    child_path(p, sizeof(p), dir, file);
    {
        FILE *f = fopen(p, "w");
        if (f) { fprintf(f, "%s\n", content); fclose(f); }
    }
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
    /* Regression: an unusable root (parent that does not exist) must fail loudly
     * with PROCESS, never silently fall through as if limits were enforced. */
    rc = cgroup_init("/nonexistent_nexus_parent_xyz/cg");
    CHECK("cgroup_init unusable root -> PROCESS", rc == NEXUS_ERR_PROCESS);
    rc = cgroup_init(root);
    CHECK("cgroup_init restores isolated root", rc == NEXUS_OK);

    /* 1b. Diagnostics are specific and never stale after success. */
    rc = cgroup_init("/nonexistent_nexus_parent_xyz/cg");
    CHECK("unusable root reports cgroup detail",
          rc == NEXUS_ERR_PROCESS && cgroup_last_error()[0] != '\0');
    rc = cgroup_init(root);
    CHECK("successful init clears stale cgroup detail",
          rc == NEXUS_OK && cgroup_last_error()[0] == '\0');
    CHECK("explicit shim root is treated as delegated",
          cgroup_self_in_delegated_root() == 1);

    /* 1c. Limit writes are verified by reading the effective value back. A
     * write to /dev/null succeeds but leaves nothing to read, so set_limits
     * must fail instead of pretending the limit is enforced. */
#ifndef _WIN32
    rc = cgroup_create("cgverify", path, sizeof(path));
    CHECK("cgroup_create cgverify", rc == NEXUS_OK);
    {
        char mem[NEXUS_PATH_MAX + 32];
        child_path(mem, sizeof(mem), path, "memory.max");
        (void)remove(mem);
        if (symlink("/dev/null", mem) == 0) {
            rc = cgroup_set_limits(path, 4096, 10);
            CHECK("set_limits rejects non-roundtripping memory.max",
                  rc == NEXUS_ERR_PROCESS);
            CHECK("read-back failure names memory.max",
                  strstr(cgroup_last_error(), "memory.max") != NULL);
            (void)remove(mem);
        } else {
            printf("SKIP: symlink unavailable for read-back test\n");
        }
    }
    (void)cgroup_destroy(path);
#endif

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

    /* Temp-root fixtures: set_limits writes memory.max/cpu.max, but the
     * required memory.current (and optional cpu.stat) only exist in a real
     * kernel cgroup. Provide controlled values so the roundtrip is testable
     * without claiming kernel behavior. */
    write_metric(path, "memory.current", "1024");
    write_metric(path, "cpu.stat", "usage_usec 4242");

    rc = cgroup_read_metrics(path, &m);
    CHECK("read_metrics cg1 OK", rc == NEXUS_OK);
    CHECK("memory_max roundtrips", m.memory_max == 256UL * 1024 * 1024);
    CHECK("memory_current reads fixture", m.memory_current == 1024UL);
    CHECK("cpu usage reads fixture", m.cpu_usage_usec == 4242UL);
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
        CHECK("attach failure names cgroup.procs",
              strstr(cgroup_last_error(), "cgroup.procs") != NULL);
        rc = cgroup_set_limits(missing, 1024, 10);
        CHECK("set_limits missing cgroup -> PROCESS", rc == NEXUS_ERR_PROCESS);
        CHECK("set_limits failure names memory.max",
              strstr(cgroup_last_error(), "memory.max") != NULL);
    }

    /* 7b. cgroup_count_procs distinguishes empty, unreadable, and populated. */
    rc = cgroup_create("cgcount", path, sizeof(path));
    CHECK("cgroup_create cgcount", rc == NEXUS_OK);
    child_path(fp, sizeof(fp), path, "cgroup.procs");
    (void)remove(fp);
    CHECK("count_procs unreadable file -> -1", cgroup_count_procs(path) == -1);
    {
        FILE *f = fopen(fp, "w");
        if (f) { fprintf(f, "111\n222\n333\n"); fclose(f); }
    }
    CHECK("count_procs counts listed pids", cgroup_count_procs(path) == 3);
    {
        FILE *f = fopen(fp, "w"); /* present but empty */
        if (f) fclose(f);
    }
    CHECK("count_procs empty file -> 0", cgroup_count_procs(path) == 0);
    CHECK("count_procs NULL -> -1", cgroup_count_procs(NULL) == -1);
    rc = cgroup_destroy(path);
    CHECK("cgcount destroy", rc == NEXUS_OK);

    /* 7c. Attach verification: membership is observable via count_procs. */
    rc = cgroup_create("cgattach", path, sizeof(path));
    CHECK("cgroup_create cgattach", rc == NEXUS_OK);
    rc = cgroup_attach(path, (long)getpid_compat());
    CHECK("attach live self", rc == NEXUS_OK);
    CHECK("attached cgroup reports >=1 proc", cgroup_count_procs(path) >= 1);
    child_path(fp, sizeof(fp), path, "cgroup.procs");
    (void)remove(fp);
    rc = cgroup_destroy(path);
    CHECK("cgattach destroy", rc == NEXUS_OK);

    /* 7d. Extended optional metrics parse when present; absent -> flags 0. */
    rc = cgroup_create("cgext", path, sizeof(path));
    CHECK("cgroup_create cgext", rc == NEXUS_OK);
    rc = cgroup_set_limits(path, 1024, 25);
    CHECK("cgext limits", rc == NEXUS_OK);
    write_metric(path, "memory.current", "1024");
    write_metric(path, "cpu.max", "25000 100000");
    write_metric(path, "memory.pressure",
                 "some avg10=1.25 avg60=0.50 avg300=0.10 total=1234");
    write_metric(path, "memory.events",
                 "low 0\nhigh 0\nmax 3\noom 2\noom_kill 1");
    rc = cgroup_read_metrics(path, &m);
    CHECK("read_metrics cgext OK", rc == NEXUS_OK);
    CHECK("cpu.max quota parsed",
          m.has_cpu_limit == 1 && m.cpu_quota_us == 25000UL);
    CHECK("cpu.max period parsed", m.cpu_period_us == 100000UL);
    CHECK("memory.pressure some avg10 parsed",
          m.has_pressure == 1 && m.pressure_some_avg10 > 1.2 &&
              m.pressure_some_avg10 < 1.3);
    CHECK("memory.events oom counters parsed",
          m.has_memory_events == 1 && m.memory_oom_events == 2 &&
              m.memory_oom_kill_events == 1);
    /* cpu.max = "max" means unlimited: no finite limit (still readable). */
    write_metric(path, "cpu.max", "max 100000");
    rc = cgroup_read_metrics(path, &m);
    CHECK("cpu.max 'max' -> no finite limit",
          rc == NEXUS_OK && m.has_cpu_limit == 0 && m.cpu_quota_us == 0);
    /* Remove the optional files: flags must read 0, not fabricated values. */
    child_path(fp, sizeof(fp), path, "cpu.max");
    (void)remove(fp);
    child_path(fp, sizeof(fp), path, "memory.pressure");
    (void)remove(fp);
    child_path(fp, sizeof(fp), path, "memory.events");
    (void)remove(fp);
    rc = cgroup_read_metrics(path, &m);
    CHECK("absent optional files -> flags 0",
          rc == NEXUS_OK && m.has_cpu_limit == 0 && m.has_pressure == 0 &&
              m.has_memory_events == 0);
    rc = cgroup_destroy(path);
    CHECK("cgext destroy", rc == NEXUS_OK);

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
