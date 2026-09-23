/*
 * NexusDB — placeholder child process (see process.h for design).
 */
#include "process.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define NEXUS_MAX_TRACKED 64

/* pid -> HANDLE table so stop() can terminate + CloseHandle. */
static LONG g_pids[NEXUS_MAX_TRACKED];
static HANDLE g_handles[NEXUS_MAX_TRACKED];
static int g_used[NEXUS_MAX_TRACKED];

static void track_add(LONG pid, HANDLE h)
{
    int i;
    for (i = 0; i < NEXUS_MAX_TRACKED; i++) {
        if (!g_used[i]) {
            g_used[i] = 1;
            g_pids[i] = pid;
            g_handles[i] = h;
            return;
        }
    }
    /* Table full: handle leaks by design here is worse than not tracking,
     * but this only bounds the demo; close to avoid an HD leak. */
    CloseHandle(h);
}

static HANDLE track_take(LONG pid)
{
    int i;
    for (i = 0; i < NEXUS_MAX_TRACKED; i++) {
        if (g_used[i] && g_pids[i] == pid) {
            HANDLE h = g_handles[i];
            g_used[i] = 0;
            g_handles[i] = NULL;
            return h;
        }
    }
    return NULL;
}

long process_spawn_placeholder(void)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    /* ~1h idle stand-in for the DB engine process. */
    char cmd[] = "C:\\Windows\\System32\\ping.exe -n 3600 127.0.0.1";

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    track_add((LONG)pi.dwProcessId, pi.hProcess);
    return (long)pi.dwProcessId;
}

int process_stop(long pid)
{
    HANDLE h;
    DWORD code;

    if (pid <= 0)
        return -1;
    h = track_take(pid);
    if (h == NULL) {
        /* Unknown pid: try to open it (e.g. spawned before? no) — else fail. */
        h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
        if (h == NULL)
            return -1;
    }
    TerminateProcess(h, 0);
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
    (void)code;
    return 0;
}

int process_is_alive(long pid)
{
    HANDLE h;
    DWORD code;

    if (pid <= 0)
        return 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h == NULL)
        return 0;
    if (!GetExitCodeProcess(h, &code)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return code == STILL_ACTIVE;
}

#else /* Linux / POSIX */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

long process_spawn_placeholder(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Child: replace image with a long-lived idler. If exec fails
         * (no sleep binary), fall back to idling in-process. */
        execlp("sleep", "sleep", "3600", (char *)NULL);
        for (;;) {
            pause();
        }
        _exit(127); /* unreachable */
    }
    return (long)pid;
}

int process_stop(long pid)
{
    pid_t c = (pid_t)pid;
    int status;
    pid_t w;
    int tries;

    if (pid <= 0)
        return -1;

    /* Ask nicely first; ignore ESRCH (already gone). */
    if (kill(c, SIGTERM) < 0 && errno != ESRCH)
        return -1;

    /* Grace period ~2s, reaping if it exits. */
    for (tries = 0; tries < 20; tries++) {
        w = waitpid(c, &status, WNOHANG);
        if (w == c)
            return 0;               /* reaped */
        if (w < 0 && errno == ECHILD)
            return 0;               /* already reaped/gone */
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100 * 1000 * 1000; /* 100ms */
            nanosleep(&ts, NULL);
        }
    }

    /* Still there: force kill, then blocking reap. */
    (void)kill(c, SIGKILL);
    w = waitpid(c, &status, 0);
    if (w == c || (w < 0 && errno == ECHILD))
        return 0;
    return -1;
}

int process_is_alive(long pid)
{
    pid_t c = (pid_t)pid;
    if (pid <= 0)
        return 0;
    if (kill(c, 0) == 0)
        return 1; /* includes zombie children; stop() reaps them */
    return errno != ESRCH ? 1 : 0;
}

#endif
