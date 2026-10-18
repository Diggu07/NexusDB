/*
 * NexusDB — Container Engine lifecycle test/demo.
 *
 * Exercises: create -> start -> status -> stop -> delete,
 * plus error paths (duplicate, bad name, bad limits, illegal states).
 *
 * Exit 0 on success, 1 on any failure. Prints PASS/FAIL per check so the
 * demo doubles as a readable walkthrough.
 */
/* POSIX declarations (readlink, rmdir, waitpid) must be exposed before any
 * system header under -std=c11 (strict ANSI). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "container/container.h"
#include "container/cgroup.h"
#include "container/namespace.h"
#include "container/process.h"

static int g_failures = 0;
static int g_skipped = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

#define SKIP(desc) do { \
    printf("ENV-LIMITED: %s\n", desc); g_skipped++; \
} while (0)

#ifndef _WIN32
static void read_ns(char *out, size_t out_len, const char *path)
{
    ssize_t n;
    out[0] = '\0';
    n = readlink(path, out, out_len - 1);
    if (n > 0)
        out[n] = '\0';
}
#endif

int main(void)
{
    char buf[1024];
    int rc;
    const nexus_container_t *c;
    nexus_cgroup_metrics_t metrics;
    nexus_namespace_config_t ns_cfg;

    printf("=== NexusDB Container Engine: lifecycle test ===\n\n");

    /* Part 1: explicit init (respects NEXUS_CGROUP_ROOT for Linux CI without
     * sudo). Must succeed or resource enforcement cannot be trusted. */
    rc = cgroup_init(NULL);
    CHECK("cgroup_init root usable", rc == NEXUS_OK);

    /* 0. Validation: bad names / bad limits are rejected. */
    rc = container_create("", 0, 0, NULL);
    CHECK("reject empty name", rc == NEXUS_ERR_INVALID);
    rc = container_create("bad name!", 0, 0, NULL);
    CHECK("reject name with space/!", rc == NEXUS_ERR_INVALID);
    rc = container_create("okname", 0, 101, NULL);
    CHECK("reject cpu > 100", rc == NEXUS_ERR_INVALID);

    /* 1. CREATE. No db_engine configured: this first container drives the
     * built-in placeholder so the lifecycle checks below stay independent of
     * the DB-engine binary. A configured db_engine is exercised in section 11. */
    rc = container_create("db1", 256UL * 1024 * 1024, 50, NULL);
    CHECK("create db1 (256MB, cpu 50)", rc == NEXUS_OK);
    c = container_get("db1");
    CHECK("registry holds db1 in CREATED with no pid",
          c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
    CHECK("db1 uses the placeholder (no db_engine configured)",
          c != NULL && c->db_engine[0] == '\0');

    /* 2. Duplicate create must fail, not corrupt state. */
    rc = container_create("db1", 0, 0, NULL);
    CHECK("duplicate create rejected (EXISTS)", rc == NEXUS_ERR_EXISTS);

    /* 3. STATUS while created. */
    rc = container_status("db1", buf, sizeof(buf));
    CHECK("status db1 works while CREATED", rc == NEXUS_OK);
    printf("--- status (CREATED) ---\n%s--------------------------\n", buf);

    /* 4. START (spawns placeholder child and attaches to cgroup). */
    rc = container_start("db1");
    CHECK("start db1", rc == NEXUS_OK);
    c = container_get("db1");
    CHECK("db1 RUNNING with live pid", c != NULL &&
          c->state == NEXUS_RUNNING && c->pid > 0);
    rc = container_start("db1");
    CHECK("second start rejected (STATE)", rc == NEXUS_ERR_STATE);
    rc = container_status("db1", buf, sizeof(buf));
    CHECK("status db1 works while RUNNING", rc == NEXUS_OK);
    printf("--- status (RUNNING) ---\n%s-------------------------\n", buf);

    /* 4.1 Cgroup metrics query (powers Rehan's OS-DB bridge). */
    rc = container_metrics("db1", &metrics);
    CHECK("container_metrics reads telemetry", rc == NEXUS_OK);
    printf("--- telemetry ---\nMemory max: %lu | Current: %lu | Pressure: %.2f\n-----------------\n",
           metrics.memory_max, metrics.memory_current, metrics.memory_pressure);

#ifndef _WIN32
    /* 4.2 The started child must actually be inside its own cgroup. Verified
     * end-to-end via /proc/<pid>/cgroup -- the attach-before-exec guarantee. */
    {
        int found = 0;
        c = container_get("db1");
        if (c != NULL && c->pid > 0) {
            char procpath[64];
            char line[512];
            FILE *f;
            snprintf(procpath, sizeof(procpath), "/proc/%ld/cgroup", c->pid);
            f = fopen(procpath, "r");
            if (f != NULL) {
                while (fgets(line, sizeof(line), f) != NULL) {
                    if (strstr(line, "/db1") != NULL)
                        found = 1;
                }
                fclose(f);
            }
        }
        CHECK("started child is inside its cgroup (/proc/<pid>/cgroup)",
              found == 1);
    }
#endif

    /* 5. DELETE while running must be refused (no orphans). */
    rc = container_delete("db1");
    CHECK("delete while RUNNING rejected (STATE)", rc == NEXUS_ERR_STATE);

    /* 6. STOP (terminates + reaps child: no zombies). */
    rc = container_stop("db1");
    CHECK("stop db1", rc == NEXUS_OK);
    c = container_get("db1");
    CHECK("db1 STOPPED with pid cleared", c != NULL &&
          c->state == NEXUS_STOPPED && c->pid == -1);
    rc = container_stop("db1");
    CHECK("second stop rejected (STATE)", rc == NEXUS_ERR_STATE);

    /* 7. DELETE, then status shows the DELETED tombstone. */
    rc = container_delete("db1");
    CHECK("delete db1", rc == NEXUS_OK);
    rc = container_status("db1", buf, sizeof(buf));
    CHECK("status after delete shows DELETED", rc == NEXUS_OK &&
          strstr(buf, "DELETED") != NULL);
    printf("--- status (DELETED) ---\n%s-------------------------\n", buf);

    /* 8. Re-create over a tombstone works (slot reuse). */
    rc = container_create("db1", 128UL * 1024 * 1024, 25, NULL);
    CHECK("re-create db1 after delete", rc == NEXUS_OK);
    rc = container_start("db1");
    CHECK("start re-created db1", rc == NEXUS_OK);
    rc = container_stop("db1");
    CHECK("stop re-created db1", rc == NEXUS_OK);
    rc = container_delete("db1");
    CHECK("delete re-created db1", rc == NEXUS_OK);

#ifndef _WIN32
    /* 8.5 If the cgroup vanishes before start, the still-blocked child must be
     * aborted: start fails, state stays CREATED, pid stays -1, no zombie. */
    {
        int rc2 = container_create("orphan", 64UL * 1024 * 1024, 25, NULL);
        CHECK("create orphan for attach-failure test", rc2 == NEXUS_OK);
        if (rc2 == NEXUS_OK) {
            c = container_get("orphan");
            if (c != NULL) {
                char cgpath[NEXUS_PATH_MAX];
                snprintf(cgpath, sizeof(cgpath), "%s", c->cgroup_path);
                CHECK("remove orphan cgroup out from under start",
                      rmdir(cgpath) == 0);
            }
            rc = container_start("orphan");
            CHECK("start with missing cgroup -> NEXUS_ERR_PROCESS",
                  rc == NEXUS_ERR_PROCESS);
            c = container_get("orphan");
            CHECK("orphan stays CREATED with pid -1 after failed start",
                  c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
            {
                int wst;
                pid_t w = waitpid(-1, &wst, WNOHANG);
                CHECK("no zombie after failed attach",
                      w == -1 && errno == ECHILD);
            }
            rc = container_delete("orphan");
            CHECK("cleanup orphan", rc == NEXUS_OK);
        }
    }
#endif

    /* 9. Unknown names. */
    rc = container_start("ghost");
    CHECK("start unknown -> NOT_FOUND", rc == NEXUS_ERR_NOT_FOUND);
    rc = container_delete("ghost");
    CHECK("delete unknown -> NOT_FOUND", rc == NEXUS_ERR_NOT_FOUND);
    rc = container_metrics("ghost", &metrics);
    CHECK("metrics unknown -> NOT_FOUND", rc == NEXUS_ERR_NOT_FOUND);

    /* 10. Namespace configuration + end-to-end integration. */
    namespace_config_default(&ns_cfg, "nsdb1");
    CHECK("namespace default has PID isolation", ns_cfg.isolate_pid == 1);
    CHECK("namespace default has Mount isolation", ns_cfg.isolate_mount == 1);
    CHECK("namespace default has Net isolation", ns_cfg.isolate_net == 1);
    CHECK("namespace default has UTS isolation", ns_cfg.isolate_uts == 1);
    CHECK("namespace default has IPC isolation", ns_cfg.isolate_ipc == 1);

#ifdef _WIN32
    CHECK("Windows namespace config maps to no clone flags",
          namespace_clone_flags(&ns_cfg) == 0);
    /* Explicit: Windows has no namespaces, so a namespaced start must refuse
     * (never a false success) and leave the container unstarted. */
    rc = container_create_ex("nsdb1", 64UL * 1024 * 1024, 25, NULL, &ns_cfg);
    CHECK("create ns container on Windows", rc == NEXUS_OK);
    rc = container_start("nsdb1");
    CHECK("Windows start with namespaces -> NEXUS_ERR_UNSUPPORTED",
          rc == NEXUS_ERR_UNSUPPORTED);
    c = container_get("nsdb1");
    CHECK("Windows ns container stays CREATED with pid -1",
          c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
    rc = container_delete("nsdb1");
    CHECK("cleanup Windows ns container", rc == NEXUS_OK);
#else
    CHECK("default namespace config maps to a non-zero clone mask",
          namespace_clone_flags(&ns_cfg) != 0);
    /* Namespace-enabled lifecycle. When the environment grants CAP_SYS_ADMIN
     * (e.g. run under `unshare -U --map-root-user`) isolation and cgroup
     * membership are proven on the live workload; otherwise the isolation
     * checks are reported ENV-LIMITED and only failure semantics are asserted. */
    rc = container_create_ex("nsdb1", 64UL * 1024 * 1024, 25, NULL, &ns_cfg);
    CHECK("create namespace-enabled container", rc == NEXUS_OK);
    c = container_get("nsdb1");
    CHECK("record captured the namespace request",
          c != NULL && c->ns_pid == 1 && c->ns_mount == 1 &&
          c->ns_net == 1 && c->ns_uts == 1 && c->ns_ipc == 1 &&
          strcmp(c->ns_hostname, "nsdb1") == 0);

    rc = container_start("nsdb1");
    if (rc == NEXUS_ERR_PROCESS) {
        SKIP("namespace-enabled start denied (need CAP_SYS_ADMIN / user namespace)");
        c = container_get("nsdb1");
        CHECK("denied ns start stays CREATED with pid -1",
              c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
        {
            int wst;
            pid_t w = waitpid(-1, &wst, WNOHANG);
            CHECK("no zombie after denied namespace start",
                  w == -1 && errno == ECHILD);
        }
    } else {
        CHECK("namespace-enabled start", rc == NEXUS_OK);
        c = container_get("nsdb1");
        CHECK("ns container RUNNING with a live pid",
              c != NULL && c->state == NEXUS_RUNNING && c->pid > 0);
        if (c != NULL && c->pid > 0) {
            char procpath[64];
            char line[512];
            int in_cg = 0;
            FILE *f;

            snprintf(procpath, sizeof(procpath), "/proc/%ld/cgroup", c->pid);
            f = fopen(procpath, "r");
            if (f != NULL) {
                while (fgets(line, sizeof(line), f) != NULL) {
                    if (strstr(line, "/nsdb1") != NULL)
                        in_cg = 1;
                }
                fclose(f);
            }
            CHECK("namespaced child is inside its cgroup (attach precedes exec)",
                  in_cg == 1);

            {
                char selfns[128];
                char childns[128];
                char p[64];
                read_ns(selfns, sizeof(selfns), "/proc/self/ns/pid");
                snprintf(p, sizeof(p), "/proc/%ld/ns/pid", c->pid);
                read_ns(childns, sizeof(childns), p);
                CHECK("namespaced child is in a new PID namespace",
                      selfns[0] != '\0' && childns[0] != '\0' &&
                      strcmp(selfns, childns) != 0);
            }
        }
        rc = container_stop("nsdb1");
        CHECK("stop namespaced container", rc == NEXUS_OK);
        c = container_get("nsdb1");
        CHECK("namespaced container STOPPED with pid cleared",
              c != NULL && c->state == NEXUS_STOPPED && c->pid == -1);
    }
    rc = container_delete("nsdb1");
    CHECK("delete namespaced container", rc == NEXUS_OK);
#endif

    /* 11. Configured DB-engine workload (P3). When a db_engine is configured,
     * container_start launches it (gated, in any requested namespaces, attached
     * to the cgroup) instead of the placeholder. The engine binary path comes
     * from $NEXUSDB_DB_ENGINE (set by the build); without it the launch checks
     * are reported ENV-LIMITED, never assumed to pass. */
#ifdef _WIN32
    rc = container_create("eng1", 64UL * 1024 * 1024, 25, "db_engine.exe");
    CHECK("create container with configured db_engine on Windows",
          rc == NEXUS_OK);
    rc = container_start("eng1");
    CHECK("Windows start with configured db_engine -> UNSUPPORTED",
          rc == NEXUS_ERR_UNSUPPORTED);
    c = container_get("eng1");
    CHECK("Windows configured-engine container stays CREATED with pid -1",
          c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
    rc = container_delete("eng1");
    CHECK("cleanup Windows configured-engine container", rc == NEXUS_OK);
#else
    {
        const char *engine = getenv("NEXUSDB_DB_ENGINE");
        char droot[256];
        char dbdir[320];
        char dbfile[512];

        if (engine == NULL || engine[0] == '\0' || access(engine, X_OK) != 0) {
            SKIP("configured db_engine binary unavailable (set NEXUSDB_DB_ENGINE)");
        } else {
            snprintf(droot, sizeof(droot), "/tmp/nexus_eng_%ld", (long)getpid());
            setenv("NEXUSDB_DATA_ROOT", droot, 1);

            rc = container_create_ex("eng1", 64UL * 1024 * 1024, 25,
                                     engine, NULL);
            CHECK("create container with configured db_engine", rc == NEXUS_OK);
            c = container_get("eng1");
            CHECK("configured db_engine path recorded",
                  c != NULL && strcmp(c->db_engine, engine) == 0);

            rc = container_start("eng1");
            c = container_get("eng1");
            CHECK("start configured db_engine", rc == NEXUS_OK);
            CHECK("configured engine RUNNING with live pid",
                  c != NULL && c->state == NEXUS_RUNNING && c->pid > 0);

            if (c != NULL && c->pid > 0) {
                struct timespec ts;
                long epid = c->pid;

                /* Let the engine initialize, then confirm it is still alive:
                 * it must be a long-running workload, not an immediate exit. */
                ts.tv_sec = 0;
                ts.tv_nsec = 300 * 1000 * 1000;
                nanosleep(&ts, NULL);
                CHECK("configured engine stays alive (long-running)",
                      process_is_alive(epid) != 0);

                {
                    char procpath[64];
                    char line[512];
                    int in_cg = 0;
                    FILE *f;
                    snprintf(procpath, sizeof(procpath), "/proc/%ld/cgroup", epid);
                    f = fopen(procpath, "r");
                    if (f != NULL) {
                        while (fgets(line, sizeof(line), f) != NULL) {
                            if (strstr(line, "/eng1") != NULL)
                                in_cg = 1;
                        }
                        fclose(f);
                    }
                    CHECK("configured engine is inside its cgroup", in_cg == 1);
                }

                snprintf(dbdir, sizeof(dbdir), "%s/eng1", droot);
                snprintf(dbfile, sizeof(dbfile), "%s/nexusdb.db", dbdir);
                CHECK("configured engine created its data directory",
                      access(dbdir, F_OK) == 0);
                CHECK("configured engine created its database file",
                      access(dbfile, F_OK) == 0);
            }

            rc = container_stop("eng1");
            CHECK("stop configured engine (clean SIGTERM shutdown)",
                  rc == NEXUS_OK);
            c = container_get("eng1");
            CHECK("configured engine STOPPED with pid cleared",
                  c != NULL && c->state == NEXUS_STOPPED && c->pid == -1);

            rc = container_delete("eng1");
            CHECK("delete configured-engine container", rc == NEXUS_OK);

            /* Best-effort cleanup of the engine's data directory. */
            (void)unlink(dbfile);
            (void)rmdir(dbdir);
            (void)rmdir(droot);
        }

        /* Invalid engine path: the gated exec fails, start aborts cleanly. */
        rc = container_create("bogus_eng", 64UL * 1024 * 1024, 25,
                              "/nonexistent/nexus_db_engine_xyz");
        CHECK("create container with bogus db_engine path", rc == NEXUS_OK);
        rc = container_start("bogus_eng");
        CHECK("start with missing db_engine -> NEXUS_ERR_PROCESS",
              rc == NEXUS_ERR_PROCESS);
        c = container_get("bogus_eng");
        CHECK("bogus-engine container stays CREATED with pid -1",
              c != NULL && c->state == NEXUS_CREATED && c->pid == -1);
        {
            int wst;
            pid_t w = waitpid(-1, &wst, WNOHANG);
            CHECK("no zombie after failed engine exec",
                  w == -1 && errno == ECHILD);
        }
        rc = container_delete("bogus_eng");
        CHECK("cleanup bogus-engine container", rc == NEXUS_OK);
    }
#endif

    printf("\n=== %s (%d failure(s), %d env-limited) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
           g_skipped);
    return g_failures == 0 ? 0 : 1;
}
