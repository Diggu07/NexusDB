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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
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

/* Runs one shell command with stdout+stderr redirected into `outfile` and
 * returns its exit code (or -1). Used where the engine's own messages (not
 * just its exit code) are part of the contract; paths must not contain
 * single quotes (spaces are fine). */
static int run_capture(const char *engine, const char *args,
                       const char *outfile, char *out, size_t outsz)
{
    char cmd[1024];
    FILE *f;
    size_t n;

    snprintf(cmd, sizeof(cmd), "'%s' %s > '%s' 2>&1", engine, args, outfile);
    const int raw = system(cmd);
    f = fopen(outfile, "rb");
    if (f == NULL) {
        out[0] = '\0';
    } else {
        n = fread(out, 1, outsz - 1, f);
        out[n] = '\0';
        (void)fclose(f);
    }
    return raw < 0 ? -1 : WEXITSTATUS(raw);
}

/* Sends one request line to the listening Unix socket at `path` and copies the
 * reply into `reply`. Connects with a short retry window. 0 on success. */
static int control_request(const char *path, const char *request,
                           char *reply, size_t replysz)
{
    struct sockaddr_un addr;
    char buf[512];
    char req[256];
    size_t got = 0;
    int fd;

    if (path == NULL || request == NULL || reply == NULL || replysz < 2)
        return -1;
    reply[0] = '\0';

    fd = -1;
    for (int attempt = 0; attempt < 40 && fd < 0; ++attempt) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            sleep_ms(50);
            continue;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof(addr.sun_path)) {
            close(fd);
            return -1;
        }
        memcpy(addr.sun_path, path, strlen(path) + 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
        close(fd);
        fd = -1;
        sleep_ms(50);
    }
    if (fd < 0)
        return -1;

    snprintf(req, sizeof(req), "%s\n", request);
    if (write(fd, req, strlen(req)) != (ssize_t)strlen(req)) {
        close(fd);
        return -1;
    }
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        if (got < replysz - 1) {
            size_t copy = (size_t)r > replysz - 1 - got ? replysz - 1 - got
                                                       : (size_t)r;
            memcpy(reply + got, buf, copy);
            got += copy;
            reply[got] = '\0';
        }
    }
    close(fd);
    return 0;
}

/* Reads the first line of `path` into `out`. 0 on success. */
static int read_first_line(const char *path, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    if (fgets(out, (int)outsz, f) == NULL) {
        (void)fclose(f);
        return -1;
    }
    out[strcspn(out, "\r\n")] = '\0';
    (void)fclose(f);
    return out[0] != '\0' ? 0 : -1;
}

/* Probes whether the filesystem holding `dir` refuses AF_UNIX bind(2) with the
 * "not supported" errnos that trigger the engine's socket relocation.
 * Returns 1 = relocation case, 0 = sockets supported, -1 = other/inconclusive. */
static int unix_fs_rejects_sockets(const char *dir)
{
    char probe[512];
    struct sockaddr_un addr;
    int fd;
    int e;
    int rc;

    snprintf(probe, sizeof(probe), "%s/.nexus_engine_probe.sock", dir);
    (void)unlink(probe);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (strlen(probe) >= sizeof(addr.sun_path)) {
        (void)close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, probe, strlen(probe) + 1);
    rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    e = errno;
    (void)close(fd);
    (void)unlink(probe);
    if (rc == 0)
        return 0;
    if (e == EOPNOTSUPP || e == EPROTONOSUPPORT)
        return 1;
    return -1;
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

            /* E. Control socket contract: the engine listens at
             * <data-dir>/nexusdb.sock, replaces a stale non-socket file
             * there, answers `ping`, and removes the socket on clean
             * shutdown. Exit code 6 pins the engine's own fail-fast value so
             * a later change that makes socket failure fatal-loud never
             * silently reverts to "unavailable-but-ready". */
            {
                char sroot[256];
                char sockpath[512];
                char reply[256];
                const char *argv[4];
                process_exec_t spec;
                process_status_t st;
                long pid;
                FILE *junk;
                int exist;

                snprintf(sroot, sizeof(sroot), "/tmp/nexus_dbeng_soc_%ld",
                         (long)getpid());
                snprintf(sockpath, sizeof(sockpath), "%s/nexusdb.sock", sroot);
                (void)rmdir(sroot);
                (void)mkdir(sroot, 0700);
                (void)unlink(sockpath);
                /* A stale non-socket file at the socket path must not stop
                 * the engine: it unlinks before bind. */
                junk = fopen(sockpath, "wb");
                if (junk != NULL) {
                    (void)fputs("stale-data", junk);
                    (void)fclose(junk);
                }

                argv[0] = engine;
                argv[1] = "--data-dir";
                argv[2] = sroot;
                argv[3] = NULL;
                spec.path = engine;
                spec.argv = argv;
                spec.cwd = NULL;
                pid = process_spawn_exec(&spec);
                CHECK("spawn db_engine for socket test", pid > 0);

                if (pid > 0) {
                    /* poll for the socket file (up to ~2s) */
                    exist = -1;
                    for (int i = 0; i < 40; ++i) {
                        exist = access(sockpath, F_OK);
                        if (exist == 0)
                            break;
                        sleep_ms(50);
                    }
                    CHECK("db_engine created its control socket",
                          exist == 0);
                    CHECK("client ping -> OK pong",
                          control_request(sockpath, "ping", reply,
                                          sizeof(reply)) == 0 &&
                              strncmp(reply, "OK pong", 7) == 0);

                    CHECK("send SIGTERM to db_engine",
                          kill(pid, SIGTERM) == 0);
                    memset(&st, 0, sizeof(st));
                    CHECK("wait for db_engine", process_wait(pid, &st) == 0);
                    CHECK("db_engine exits 0 on SIGTERM (clean shutdown)",
                          st.exited && st.exit_code == 0);
                    CHECK("control socket removed on clean shutdown",
                          access(sockpath, F_OK) != 0);
                }
                (void)unlink(sockpath);
                (void)rmdir(sroot);
            }

            /* F. A socket the engine cannot bind is fatal: exit code 6, the
             * exact failing syscall with its errno on stderr, and never a
             * ready line. /dev/null/nexusdb.sock fails bind() with ENOTDIR
             * on every POSIX system (/socket() succeeds, so only real
             * bind/listen errors are exercised). */
            {
                char froot[256];
                char fargs[512];
                char fsocket[64];
                char output[4096];
                char outfile[300];

                snprintf(froot, sizeof(froot), "/tmp/nexus_dbeng_socf_%ld",
                         (long)getpid());
                (void)rmdir(froot);
                snprintf(fargs, sizeof(fargs), "--data-dir '%s' --socket /dev/null/nexusdb.sock", froot);
                snprintf(outfile, sizeof(outfile), "%s.out", froot);
                (void)unlink(outfile);

                snprintf(fsocket, sizeof(fsocket), "errno=%d", ENOTDIR);
                {
                    const int rc = run_capture(engine, fargs, outfile, output,
                                               sizeof(output));
                    CHECK("un-bindable socket -> exit 6 (fail fast)", rc == 6);
                }
                CHECK("failure names the control socket and path",
                      strstr(output, "control socket unavailable") != NULL &&
                          strstr(output, "/dev/null/nexusdb.sock") != NULL);
                CHECK("failure reports the failing syscall (bind)",
                      strstr(output, "bind failed") != NULL);
                CHECK("failure reports the errno name and number",
                      strstr(output, "Not a directory") != NULL &&
                          strstr(output, fsocket) != NULL);
                CHECK("no ready line on socket failure",
                      strstr(output, "db_engine: ready") == NULL);
                (void)unlink(outfile);
                (void)rmdir(froot);
            }

            /* G. Filesystems that cannot host Unix sockets at all (WSL2
             * drvfs/9p mounts under /mnt/<drive>): the default socket must
             * be relocated to a directory that supports AF_UNIX, its real
             * location recorded in <data-dir>/nexusdb.sock.path, and pings
             * answered there. ENV-LIMITED when the environment offers no
             * such filesystem. */
            {
                const char *groot = "/mnt/d";
                char gdb[300];
                char marker[340];
                char realsock[340];
                char reply[256];
                char dirbuf[512];
                char gfile[520];
                const char *argv[4];
                process_exec_t spec;
                process_status_t st;
                long pid;
                int verdict;

                if (access(groot, F_OK) != 0) {
                    SKIP("no /mnt/d present (socket relocation untested here)");
                } else if (access(groot, W_OK) != 0) {
                    SKIP("/mnt/d not writable (socket relocation untested here)");
                } else {
                    verdict = unix_fs_rejects_sockets(groot);
                    if (verdict <= 0) {
                        SKIP("/mnt/d accepts AF_UNIX bind or probe inconclusive; "
                             "socket relocation not exercised here");
                    } else {
                        snprintf(gdb, sizeof(gdb), "%s/nexus_dbeng_g_%ld",
                                 groot, (long)getpid());
                        snprintf(marker, sizeof(marker),
                                 "%s/nexusdb.sock.path", gdb);

                        /* Keep the relocated socket in a deterministic test
                         * directory instead of the user's runtime dir. */
                        snprintf(dirbuf, sizeof(dirbuf), "/tmp/nexus_dbeng_rel_%ld",
                                 (long)getpid());
                        (void)rmdir(dirbuf);
                        (void)mkdir(dirbuf, 0700);
                        setenv("NEXUSDB_SOCKET_DIR", dirbuf, 1);

                        snprintf(gfile, sizeof(gfile), "%s.out", dirbuf);
                        (void)unlink(gfile);

                        /* Live process, not run_capture: we need it serving. */
                        argv[0] = engine;
                        argv[1] = "--data-dir";
                        argv[2] = gdb;
                        argv[3] = NULL;
                        spec.path = engine;
                        spec.argv = argv;
                        spec.cwd = NULL;
                        pid = process_spawn_exec(&spec);
                        CHECK("spawn db_engine on socket-hostile filesystem",
                              pid > 0);
                        if (pid > 0) {
                            int have = -1;
                            for (int i = 0; i < 40; ++i) {
                                have = access(marker, F_OK);
                                if (have == 0)
                                    break;
                                sleep_ms(50);
                            }
                            CHECK("relocation marker written in data dir",
                                  have == 0);
                            if (read_first_line(marker, realsock,
                                                sizeof(realsock)) == 0) {
                                CHECK("marker records a /tmp socket path",
                                      strncmp(realsock, "/tmp/nexus_dbeng_rel_",
                                              21) == 0);
                                CHECK("relocated socket answers ping",
                                      control_request(realsock, "ping", reply,
                                                      sizeof(reply)) == 0 &&
                                          strncmp(reply, "OK pong", 7) == 0);
                            } else {
                                CHECK("marker readable", 0);
                            }
                            CHECK("db_engine stays alive after relocation",
                                  process_is_alive(pid) != 0);

                            CHECK("send SIGTERM to db_engine",
                                  kill(pid, SIGTERM) == 0);
                            memset(&st, 0, sizeof(st));
                            CHECK("wait for db_engine",
                                  process_wait(pid, &st) == 0);
                            CHECK("exit 0 after relocated-socket shutdown",
                                  st.exited && st.exit_code == 0);
                            CHECK("marker removed on clean shutdown",
                                  access(marker, F_OK) != 0);
                        }
                        (void)unlink(gfile);
                        (void)rmdir(dirbuf);
                        (void)rmdir(gdb);
                    }
                }
            }
        }
    }
#endif /* !_WIN32 */

    printf("\n=== %s (%d failure(s), %d env-limited) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
           g_skipped);
    return g_failures == 0 ? 0 : 1;
}
