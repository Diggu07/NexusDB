/*
 * NexusDB — real process-launch test (Part 2).
 *
 * Uses an isolated cgroup root (no sudo). Exercises the process_spawn API
 * directly (invalid args, missing executable, placeholder lifecycle) and
 * through the container lifecycle (bad db_engine fails cleanly, real
 * executable runs inside its cgroup with its pid in cgroup.procs).
 *
 * Exit 0 on success, 1 on any failure. Same PASS/FAIL style as the other
 * container suites.
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
#include <unistd.h>
#define getpid_compat() (int)getpid()
#endif

#include "container/cgroup.h"
#include "container/container.h"
#include "container/namespace.h"
#include "container/process.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

static void test_root(char *buf, size_t len)
{
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
#ifdef _WIN32
    if (!tmp) tmp = ".";
    snprintf(buf, len, "%s\\nexus_p-test_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#else
    if (!tmp) tmp = "/tmp";
    snprintf(buf, len, "%s/nexus_p-test_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#endif
}

/* 1 when the cgroup's cgroup.procs lists pid (proves self-attach).
 * Linux-only: the Windows sections never call it. */
#ifndef _WIN32
static int procs_contains(const char *cgroup_path, long pid)
{
    char fp[NEXUS_PATH_MAX + 32];
    char want[32];
    char content[1024];
    FILE *f;
    size_t n;
    snprintf(fp, sizeof(fp), "%s/cgroup.procs", cgroup_path);
    snprintf(want, sizeof(want), "%ld", pid);
    f = fopen(fp, "r");
    if (!f)
        return 0;
    n = fread(content, 1, sizeof(content) - 1, f);
    fclose(f);
    content[n] = '\0';
    return strstr(content, want) != NULL;
}
#endif

int main(void)
{
    char root[NEXUS_PATH_MAX];
    int rc;
    long pid;
    const nexus_container_t *c;

    printf("=== NexusDB real process-launch test (isolated root) ===\n\n");

    test_root(root, sizeof(root));
    printf("isolated root: %s\n", root);
    rc = cgroup_init(root);
    CHECK("cgroup_init isolated root", rc == NEXUS_OK);

    /* 1. process_spawn rejects bad arguments (no fork on invalid input). */
    {
        static char a0[] = "x";
        char *good_argv[] = {a0, NULL};
        char *null_argv0[] = {NULL};
        CHECK("spawn NULL exe -> -1",
              process_spawn(NULL, good_argv, NULL, NULL) < 0);
        CHECK("spawn empty exe -> -1",
              process_spawn("", good_argv, NULL, NULL) < 0);
        CHECK("spawn NULL argv -> -1",
              process_spawn("sleep", NULL, NULL, NULL) < 0);
        CHECK("spawn argv[0] NULL -> -1",
              process_spawn("sleep", null_argv0, NULL, NULL) < 0);
    }

    /* 2. Missing executable fails fast and honestly (no zombie, no hang). */
    {
        static char m0[] = "nexus_missing_xyz";
        char *margv[] = {m0, NULL};
        nexus_namespace_config_t ns;
        namespace_config_default(&ns, "missing");
        pid = process_spawn(
#ifdef _WIN32
            "C:\\nonexistent\\nexus_xyz_123.exe",
#else
            "/nonexistent/nexus_xyz_123",
#endif
            margv, NULL, NULL);
        CHECK("spawn missing exe -> -1", pid < 0);
        CHECK("missing exe leaves nothing alive",
              pid <= 0 || !process_is_alive(pid));
        pid = process_spawn(
#ifdef _WIN32
            "C:\\nonexistent\\nexus_xyz_123.exe",
#else
            "/nonexistent/nexus_xyz_123",
#endif
            margv, root, &ns);
        CHECK("spawn missing exe with cgroup+ns -> -1", pid < 0);
    }

    /* 3. Placeholder still works: alive after spawn, dead after stop. */
    pid = process_spawn_placeholder();
    CHECK("placeholder spawns", pid > 0);
    CHECK("placeholder alive", pid > 0 && process_is_alive(pid));
    if (pid > 0) {
        CHECK("placeholder stop", process_stop(pid) == 0);
        CHECK("placeholder dead after stop", !process_is_alive(pid));
    }

    /* 4. Container with a bad db_engine fails start cleanly. */
    rc = container_create("badexe",
#ifdef _WIN32
                          64UL * 1024 * 1024, 10, "C:\\nonexistent\\nexus_xyz.exe");
#else
                          64UL * 1024 * 1024, 10, "/nonexistent/nexus_xyz");
#endif
    CHECK("create badexe", rc == NEXUS_OK);
    rc = container_start("badexe");
    CHECK("start badexe -> PROCESS", rc == NEXUS_ERR_PROCESS);
    c = container_get("badexe");
    CHECK("badexe stays CREATED with no pid",
          c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
    rc = container_delete("badexe");
    CHECK("delete badexe", rc == NEXUS_OK);

#ifndef _WIN32
    /* 5. Linux: real executable runs inside its cgroup (self-attached before
     * exec, direct launch with no shell). Uses a tiny sleep script since the
     * container API passes argv={exe} and sleep needs a duration arg. */
    {
        char script[NEXUS_PATH_MAX + 16];
        FILE *f;
        snprintf(script, sizeof(script), "%s/idler.sh", root);
        f = fopen(script, "w");
        CHECK("idler script writable", f != NULL);
        if (f) {
            fprintf(f, "#!/bin/sh\nexec sleep 60\n");
            fclose(f);
#ifndef _WIN32
            CHECK("idler script chmod", chmod(script, 0755) == 0);
#endif
        }

        rc = container_create("realdb", 128UL * 1024 * 1024, 25, script);
        CHECK("create realdb with script exe", rc == NEXUS_OK);
        rc = container_start("realdb");
        CHECK("start realdb (real exe)", rc == NEXUS_OK);
        c = container_get("realdb");
        CHECK("realdb RUNNING with live pid",
              c != NULL && c->state == NEXUS_RUNNING && c->pid > 0 &&
              process_is_alive(c->pid));
        if (c && c->pid > 0) {
            nexus_cgroup_metrics_t m;
            CHECK("realdb pid self-attached to its cgroup",
                  procs_contains(c->cgroup_path, c->pid));
            rc = container_metrics("realdb", &m);
            CHECK("realdb metrics readable", rc == NEXUS_OK);
            printf("--- realdb: max=%lu current=%lu pressure=%.4f ---\n",
                   m.memory_max, m.memory_current, m.memory_pressure);
        }
        rc = container_stop("realdb");
        CHECK("stop realdb", rc == NEXUS_OK);
        rc = container_delete("realdb");
        CHECK("delete realdb", rc == NEXUS_OK);
        (void)remove(script);
    }
#else
    printf("(skip: real-exe cgroup test is Linux-only)\n");
#endif

    /* 6. Restart still works after a failed start (no wedged state). */
    rc = container_create("retry", 64UL * 1024 * 1024, 10, NULL);
    CHECK("create retry (placeholder)", rc == NEXUS_OK);
    rc = container_start("retry");
    CHECK("start retry", rc == NEXUS_OK);
    rc = container_stop("retry");
    CHECK("stop retry", rc == NEXUS_OK);
    rc = container_start("retry");
    CHECK("restart retry", rc == NEXUS_OK);
    rc = container_stop("retry");
    CHECK("stop retry again", rc == NEXUS_OK);
    rc = container_delete("retry");
    CHECK("delete retry", rc == NEXUS_OK);

#ifdef _WIN32
    (void)_rmdir(root);
#else
    (void)rmdir(root);
#endif

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
