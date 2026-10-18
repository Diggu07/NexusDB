/*
 * NexusDB — Container Engine: cgroups v2 implementation (Ayush: Systems Engineer)
 *
 * Provides resource isolation and real-time observability.
 * Real target: Ubuntu 22.04 / WSL2 with cgroups v2.
 * Dev target: Windows fallback shim with mock files / graceful handling.
 *
 * Part 1 reliability contract:
 *   - cgroup_init() is idempotent; subtree_control setup is best-effort.
 *   - create/set_limits/attach return errors instead of silent success.
 *   - read_metrics() requires memory.current + memory.max; missing or
 *     corrupt files return NEXUS_ERR_PROCESS (never fake zero pressure).
 *     cpu.stat is optional (missing -> usage 0).
 *   - destroy() refuses non-empty cgroups (NEXUS_ERR_STATE) and is
 *     idempotent on missing paths.
 */
#include "container/cgroup.h"
#include "container/container.h"
#include "container/process.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_compat(path) _mkdir(path)
#define rmdir_compat(path) _rmdir(path)
#else
#include <unistd.h>
#define mkdir_compat(path) mkdir(path, 0755)
#define rmdir_compat(path) rmdir(path)
#endif

static char g_cgroup_root[NEXUS_PATH_MAX] = {0};

static const char *get_root(void)
{
    if (g_cgroup_root[0] != '\0')
        return g_cgroup_root;

    const char *env = getenv("NEXUS_CGROUP_ROOT");
    if (env && env[0] != '\0') {
        snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s", env);
    } else {
#ifdef _WIN32
        const char *tmp = getenv("TEMP");
        if (!tmp) tmp = getenv("TMP");
        if (!tmp) tmp = ".";
        snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s\\nexus_cgroup", tmp);
#else
        snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s", NEXUS_CGROUP_DEFAULT_ROOT);
#endif
    }
    return g_cgroup_root;
}

const char *cgroup_root(void)
{
    return get_root();
}

static int write_file_str(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    if (fprintf(f, "%s\n", value) < 0) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/* mkdir that tolerates EEXIST-when-a-directory. Returns 0 on usable dir, -1 otherwise. */
static int mkdir_usable(const char *path)
{
    if (mkdir_compat(path) == 0)
        return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0) {
#ifdef _WIN32
            (void)st;
            return 0; /* _mkdir EEXIST on Windows implies something there; accept it */
#else
            if (S_ISDIR(st.st_mode))
                return 0;
#endif
        }
        /* EEXIST but not a directory (or stat failed): fall through to error */
    }
    /* Re-stat: concurrent creator may have made it between mkdir and here. */
    {
        struct stat st;
        if (stat(path, &st) == 0) {
#ifdef _WIN32
            return 0;
#else
            if (S_ISDIR(st.st_mode))
                return 0;
#endif
        }
    }
    return -1;
}

/* Reject traversal / separator names at the cgroup layer too (container layer
 * validates stricter charset, but the direct API must at least be safe). */
static int cgroup_name_valid(const char *name)
{
    size_t n;
    if (name == NULL)
        return 0;
    n = strlen(name);
    if (n == 0 || n >= NEXUS_NAME_MAX)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = name[i];
        if (ch == '/' || ch == '\\')
            return 0;
    }
    return 1;
}

static void build_child_path(char *buf, size_t len, const char *dir, const char *file)
{
#ifdef _WIN32
    snprintf(buf, len, "%s\\%s", dir, file);
#else
    snprintf(buf, len, "%s/%s", dir, file);
#endif
}

int cgroup_init(const char *root_override)
{
    if (root_override && root_override[0] != '\0') {
        if (strlen(root_override) >= sizeof(g_cgroup_root))
            return NEXUS_ERR_INVALID;
        snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s", root_override);
    } else {
        (void)get_root();
    }

    if (mkdir_usable(g_cgroup_root) != 0)
        return NEXUS_ERR_PROCESS;

#ifndef _WIN32
    /* Best-effort: already-enabled controllers or unprivileged hierarchies
     * must not fail init when the root itself is usable. */
    {
        /* g_cgroup_root holds up to NEXUS_PATH_MAX-1 chars; suffixing
         * "/cgroup.subtree_control" needs a buffer one suffix larger so the
         * path can never be silently truncated before being written. */
        char sub_path[NEXUS_PATH_MAX + 32];
        int n = snprintf(sub_path, sizeof(sub_path), "%s/cgroup.subtree_control", g_cgroup_root);
        if (n < 0 || (size_t)n >= sizeof(sub_path))
            return NEXUS_ERR_INVALID;
        (void)write_file_str(sub_path, "+memory +cpu");
    }
#endif

    return NEXUS_OK;
}

int cgroup_create(const char *name, char *out_path, size_t out_path_len)
{
    int n;

    if (!cgroup_name_valid(name) || !out_path || out_path_len == 0)
        return NEXUS_ERR_INVALID;

    /* Ensure root exists (EEXIST-tolerant). A missing/uncreatable root is a
     * real failure, not something to silently ignore. */
    {
        const char *root = get_root();
        if (mkdir_usable(root) != 0)
            return NEXUS_ERR_PROCESS;
    }

    {
        const char *root = get_root();
#ifdef _WIN32
        n = snprintf(out_path, out_path_len, "%s\\%s", root, name);
#else
        n = snprintf(out_path, out_path_len, "%s/%s", root, name);
#endif
        if (n < 0 || (size_t)n >= out_path_len)
            return NEXUS_ERR_INVALID;
    }

    if (mkdir_usable(out_path) != 0)
        return NEXUS_ERR_PROCESS;

#ifdef _WIN32
    /* Windows shim: the kernel does not provide cgroup files, so create mock
     * telemetry files. On Linux these are kernel-owned and must NOT be made. */
    {
        char fp[NEXUS_PATH_MAX + 32];
        build_child_path(fp, sizeof(fp), out_path, "memory.current");
        {
            FILE *f = fopen(fp, "r");
            if (!f) {
                if (write_file_str(fp, "0") != 0)
                    return NEXUS_ERR_PROCESS;
            } else {
                fclose(f);
            }
        }
        build_child_path(fp, sizeof(fp), out_path, "cpu.stat");
        {
            FILE *f = fopen(fp, "r");
            if (!f) {
                if (write_file_str(fp, "usage_usec 0") != 0)
                    return NEXUS_ERR_PROCESS;
            } else {
                fclose(f);
            }
        }
        build_child_path(fp, sizeof(fp), out_path, "cgroup.procs");
        {
            FILE *f = fopen(fp, "r");
            if (!f) {
                if (write_file_str(fp, "") != 0)
                    return NEXUS_ERR_PROCESS;
            } else {
                fclose(f);
            }
        }
    }
#endif

    return NEXUS_OK;
}

int cgroup_set_limits(const char *cgroup_path, unsigned long memory_bytes, unsigned int cpu_percent)
{
    char file_path[NEXUS_PATH_MAX + 32];
    char val[64];

    if (!cgroup_path || cgroup_path[0] == '\0')
        return NEXUS_ERR_INVALID;
    if (cpu_percent > 100)
        return NEXUS_ERR_INVALID;

    /* 1. Memory limit -> memory.max */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "memory.max");

    if (memory_bytes > 0)
        snprintf(val, sizeof(val), "%lu", memory_bytes);
    else
        snprintf(val, sizeof(val), "max");
    if (write_file_str(file_path, val) != 0)
        return NEXUS_ERR_PROCESS;

    /* 2. CPU limit -> cpu.max (quota, period) */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "cpu.max");

    if (cpu_percent > 0 && cpu_percent <= 100) {
        unsigned long period = 100000;
        unsigned long quota = (period * cpu_percent) / 100;
        snprintf(val, sizeof(val), "%lu %lu", quota, period);
    } else {
        snprintf(val, sizeof(val), "max 100000");
    }
    if (write_file_str(file_path, val) != 0)
        return NEXUS_ERR_PROCESS;

    return NEXUS_OK;
}

int cgroup_attach(const char *cgroup_path, long pid)
{
    char file_path[NEXUS_PATH_MAX + 32];
    char val[32];

    if (!cgroup_path || cgroup_path[0] == '\0' || pid <= 0)
        return NEXUS_ERR_INVALID;

    build_child_path(file_path, sizeof(file_path), cgroup_path, "cgroup.procs");

    snprintf(val, sizeof(val), "%ld", pid);
    if (write_file_str(file_path, val) != 0)
        return NEXUS_ERR_PROCESS;
    return NEXUS_OK;
}

int cgroup_read_metrics(const char *cgroup_path, nexus_cgroup_metrics_t *metrics)
{
    char file_path[NEXUS_PATH_MAX + 32];
    FILE *f;

    if (!cgroup_path || cgroup_path[0] == '\0' || !metrics)
        return NEXUS_ERR_INVALID;

    memset(metrics, 0, sizeof(*metrics));

    /* 1. memory.current (REQUIRED) */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "memory.current");
    f = fopen(file_path, "r");
    if (!f)
        return NEXUS_ERR_PROCESS;
    {
        unsigned long cur = 0;
        int scanned = fscanf(f, "%lu", &cur);
        fclose(f);
        if (scanned != 1)
            return NEXUS_ERR_PROCESS;
        metrics->memory_current = cur;
    }

    /* 2. memory.max (REQUIRED): "max" means unlimited (0), else bytes. */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "memory.max");
    f = fopen(file_path, "r");
    if (!f)
        return NEXUS_ERR_PROCESS;
    {
        char buf[32];
        int scanned = fscanf(f, "%31s", buf);
        fclose(f);
        if (scanned != 1)
            return NEXUS_ERR_PROCESS;
        if (strcmp(buf, "max") == 0) {
            metrics->memory_max = 0;
        } else {
            char *end = NULL;
            unsigned long v;
            errno = 0;
            v = strtoul(buf, &end, 10);
            if (errno != 0 || end == buf || *end != '\0')
                return NEXUS_ERR_PROCESS;
            metrics->memory_max = v;
        }
    }

    /* 3. cpu.stat (OPTIONAL): missing file or missing key yields 0. */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "cpu.stat");
    f = fopen(file_path, "r");
    if (f) {
        char key[64];
        unsigned long val = 0;
        while (fscanf(f, "%63s %lu", key, &val) == 2) {
            if (strcmp(key, "usage_usec") == 0) {
                metrics->cpu_usage_usec = val;
                break;
            }
        }
        fclose(f);
    } else {
        metrics->cpu_usage_usec = 0;
    }

    /* 4. Calculate memory pressure */
    if (metrics->memory_max > 0) {
        metrics->memory_pressure = (double)metrics->memory_current / (double)metrics->memory_max;
    } else {
        metrics->memory_pressure = 0.0;
    }

    return NEXUS_OK;
}

/* Live when cgroup.procs holds any pid that still names a live process
 * (zombies count as live until reaped). Missing/empty files, or files whose
 * pids are all dead (stale shim entries after a stop/crash), count as empty:
 * on real Linux the kernel removes exited pids itself, so this tolerance only
 * affects the Windows shim and crash-recovery paths. Unparsable non-ws
 * content is treated as live (fail closed). */
static int procs_has_live(const char *cgroup_path)
{
    char file_path[NEXUS_PATH_MAX + 32];
    FILE *f;
    int saw_content = 0;

    build_child_path(file_path, sizeof(file_path), cgroup_path, "cgroup.procs");
    f = fopen(file_path, "r");
    if (!f)
        return 0;
    for (;;) {
        char tok[64];
        int rc = fscanf(f, "%63s", tok);
        if (rc != 1)
            break;
        saw_content = 1;
        {
            char *end = NULL;
            long pid;
            errno = 0;
            pid = strtol(tok, &end, 10);
            if (errno != 0 || end == tok || *end != '\0' || pid <= 0) {
                fclose(f);
                return 1; /* unparsable: fail closed */
            }
            if (process_is_alive(pid)) {
                fclose(f);
                return 1;
            }
            /* dead pid: keep scanning; another entry may still be live */
        }
    }
    fclose(f);
    /* No tokens at all -> empty. Tokens but all dead -> stale, drainable. */
    if (saw_content) {
        /* Best-effort drain of stale shim entries; kernel files just fail. */
        f = fopen(file_path, "w");
        if (f)
            fclose(f);
    }
    return 0;
}

int cgroup_destroy(const char *cgroup_path)
{
    char file_path[NEXUS_PATH_MAX + 32];
    const char *files[] = {"memory.max", "memory.current", "cpu.max", "cpu.stat", "cgroup.procs", NULL};

    if (!cgroup_path || cgroup_path[0] == '\0')
        return NEXUS_ERR_INVALID;

    /* Never rmdir a cgroup that still owns live processes. Stale dead-pid
     * entries are drained best-effort inside procs_has_live(). */
    if (procs_has_live(cgroup_path))
        return NEXUS_ERR_STATE;

    /* Remove shim/mock files if any (kernel files just fail, ignored). */
    for (int i = 0; files[i] != NULL; ++i) {
        build_child_path(file_path, sizeof(file_path), cgroup_path, files[i]);
        (void)remove(file_path);
    }

    if (rmdir_compat(cgroup_path) != 0) {
        if (errno == ENOENT)
            return NEXUS_OK; /* idempotent: already gone */
        {
            /* A concurrent destroy may have removed it between remove()s. */
            struct stat st;
            if (stat(cgroup_path, &st) != 0 && errno == ENOENT)
                return NEXUS_OK;
        }
        return NEXUS_ERR_PROCESS;
    }
    return NEXUS_OK;
}
