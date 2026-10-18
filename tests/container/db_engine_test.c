/*
 * NexusDB — db_engine executable contract test (P3).
 *
 * Exercises the db_engine binary DIRECTLY (not through the container) using the
 * P0 direct-execution API, so the executable's own contract is pinned down:
 *   - it stays alive until asked to stop, then shuts down cleanly (SIGTERM
 *     delivers exit code 0);
 *   - it creates its data directory and database file on startup;
 *   - an unknown argument is a usage error (exit 2);
 *   - a data directory it cannot create is a storage failure (exit 3);
 *   - an existing file whose size is not a page multiple is refused (exit 3).
 *
 * The engine path comes from $NEXUSDB_DB_ENGINE (set by the build); without it,
 * or when the binary is not directly executable, the checks are reported
 * ENV-LIMITED rather than assumed to pass. On Windows the direct-execution API
 * is POSIX-only (process_spawn_exec -> ENOSYS), so the whole test is
 * ENV-LIMITED there.
 *
 * Exit 0 on success, 1 on any failure.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "container/process.h"

#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static int g_failures = 0;
static int g_skipped = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", (desc)); } \
    else { printf("FAIL: %s\n", (desc)); g_failures++; } \
} while (0)

#define SKIP(desc) do { \
    printf("ENV-LIMITED: %s\n", (desc)); g_skipped++; \
} while (0)

#ifndef _WIN32
static const char *engine_path(void)
{
    const char *e = getenv("NEXUSDB_DB_ENGINE");
    if (e != NULL && e[0] != '\0')
        return e;
    return "./db_engine";
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* Spawn `engine` with the given argv, wait for it to exit, and return its exit
 * code (or -1 when the process did not exit normally). */
static int run_and_get_exit(const char *engine, const char *const *argv)
{
    process_exec_t spec;
    process_status_t st;
    long pid;

    spec.path = engine;
    spec.argv = argv;
    spec.cwd = NULL;

    pid = process_spawn_exec(&spec);
    if (pid <= 0)
        return -1;
    memset(&st, 0, sizeof(st));
    if (process_wait(pid, &st) != 0)
        return -1;
    return st.exited ? st.exit_code : -1;
}
#endif /* !_WIN32 */

int main(void)
{
    printf("=== NexusDB db_engine executable contract test (P3) ===\n\n");

#ifdef _WIN32
    SKIP("direct-exec API is POSIX-only (process_spawn_exec -> ENOSYS)");
#else
    {
        const char *engine = engine_path();

        if (access(engine, X_OK) != 0) {
            SKIP("db_engine binary unavailable (set NEXUSDB_DB_ENGINE)");
        } else {
            char droot[256];
            char dbfile[512];

            /* A. Stays alive until a shutdown request, then exits cleanly. */
            snprintf(droot, sizeof(droot), "/tmp/nexus_dbeng_%ld",
                     (long)getpid());
            snprintf(dbfile, sizeof(dbfile), "%s/nexusdb.db", droot);
            (void)unlink(dbfile);
            (void)rmdir(droot);

            {
                process_exec_t spec;
                const char *argv[4];
                long pid;

                argv[0] = engine;
                argv[1] = "--data-dir";
                argv[2] = droot;
                argv[3] = NULL;
                spec.path = engine;
                spec.argv = argv;
                spec.cwd = NULL;

                errno = 0;
                pid = process_spawn_exec(&spec);
                CHECK("spawn db_engine directly", pid > 0);

                if (pid > 0) {
                    sleep_ms(300);
                    CHECK("db_engine stays alive until asked to stop",
                          process_is_alive(pid) != 0);
                    CHECK("db_engine created its data directory",
                          access(droot, F_OK) == 0);
                    CHECK("db_engine created its database file",
                          access(dbfile, F_OK) == 0);

                    CHECK("send SIGTERM to db_engine", kill(pid, SIGTERM) == 0);
                    {
                        process_status_t st;
                        memset(&st, 0, sizeof(st));
                        CHECK("wait for db_engine", process_wait(pid, &st) == 0);
                        CHECK("db_engine exits 0 on SIGTERM (clean shutdown)",
                              st.exited && st.exit_code == 0);
                    }
                    CHECK("db_engine gone after shutdown",
                          process_is_alive(pid) == 0);
                }

                (void)unlink(dbfile);
                (void)rmdir(droot);
            }

            /* B. Unknown argument -> usage error (exit 2). */
            {
                const char *argv[3];
                argv[0] = engine;
                argv[1] = "--no-such-flag";
                argv[2] = NULL;
                CHECK("unknown argument -> exit 2",
                      run_and_get_exit(engine, argv) == 2);
            }

            /* C. Data directory that cannot be created -> storage failure
             * (exit 3). `/dev/null/...` cannot be a directory (ENOTDIR). */
            {
                const char *argv[4];
                argv[0] = engine;
                argv[1] = "--data-dir";
                argv[2] = "/dev/null/nexus_engine_nodir";
                argv[3] = NULL;
                CHECK("uncreatable data directory -> exit 3",
                      run_and_get_exit(engine, argv) == 3);
            }

            /* D. An existing database file that is not a whole number of pages
             * is refused (exit 3): the engine must not silently misread it. */
            {
                char baddir[256];
                char badfile[512];
                const char *argv[4];
                FILE *f;

                snprintf(baddir, sizeof(baddir), "/tmp/nexus_dbeng_bad_%ld",
                         (long)getpid());
                snprintf(badfile, sizeof(badfile), "%s/nexusdb.db", baddir);
                (void)mkdir(baddir, 0700);
                f = fopen(badfile, "wb");
                if (f != NULL) {
                    char junk[100];
                    memset(junk, 0, sizeof(junk)); /* 100 B, not 4096-multiple */
                    (void)fwrite(junk, 1, sizeof(junk), f);
                    (void)fclose(f);
                }

                argv[0] = engine;
                argv[1] = "--data-dir";
                argv[2] = baddir;
                argv[3] = NULL;
                CHECK("non-page-multiple db file -> exit 3",
                      run_and_get_exit(engine, argv) == 3);

                (void)unlink(badfile);
                (void)rmdir(baddir);
            }
        }
    }
#endif /* !_WIN32 */

    printf("\n=== %s (%d failure(s), %d env-limited) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
           g_skipped);
    return g_failures == 0 ? 0 : 1;
}
