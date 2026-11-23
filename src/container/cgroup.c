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
#include <sys/types.h>
#include <unistd.h>
#define mkdir_compat(path) mkdir(path, 0755)
#define rmdir_compat(path) rmdir(path)
#endif

static char g_cgroup_root[NEXUS_PATH_MAX] = {0};
static char g_last_error[512] = {0};

static int mkdir_usable(const char *path);
static int write_file_str(const char *path, const char *value);
#ifndef _WIN32
static int dir_hosts_controllers(const char *dir);
static int delegated_cgroup_root(char *out, size_t len);
static int user_manager_cgroup_root(char *out, size_t len);
#endif

static void set_error_errno(const char *op, const char *path)
{
    const int e = errno;
    if (path != NULL && path[0] != '\0')
        snprintf(g_last_error, sizeof(g_last_error), "%s '%s': %s", op, path,
                 strerror(e));
    else
        snprintf(g_last_error, sizeof(g_last_error), "%s: %s", op, strerror(e));
}

static void set_error_text(const char *text)
{
    snprintf(g_last_error, sizeof(g_last_error), "%s", text);
}

const char *cgroup_last_error(void)
{
    return g_last_error;
}

void cgroup_clear_error(void)
{
    g_last_error[0] = '\0';
}

#ifndef _WIN32
/* A default root is only selected when it exists (or can be created) AND can
 * host controller-enabled children. Existence alone is not proof of authority:
 * /sys/fs/cgroup/nexusdb may be root-owned while this process runs as a user. */
static int root_ready(const char *dir)
{
    char sub[NEXUS_PATH_MAX + 32];
    int n;

    if (mkdir_usable(dir) != 0)
        return 0;
    n = snprintf(sub, sizeof(sub), "%s/cgroup.subtree_control", dir);
    if (n > 0 && (size_t)n < sizeof(sub))
        (void)write_file_str(sub, "+memory +cpu");
    return dir_hosts_controllers(dir);
}
#endif

/*
 * Resolve the effective cgroup root. Order of preference:
 *   1. an explicit cgroup_init()/cgroup_root() override (already cached)
 *   2. $NEXUS_CGROUP_ROOT
 *   3. the platform default root, WHEN IT IS ACTUALLY USABLE
 *   4. on Linux, a child of THIS process's own cgroup (so a runtime launched in
 *      a delegated subtree, e.g. `systemd-run --user --scope`, manages its
 *      containers inside that delegation without a hardcoded privileged path)
 *   5. otherwise the default, so the later create/set_limits call fails loudly
 *      instead of silently using a path the process cannot manage.
 */
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
        char derived[NEXUS_PATH_MAX];

        if (root_ready(NEXUS_CGROUP_DEFAULT_ROOT)) {
            snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s",
                     NEXUS_CGROUP_DEFAULT_ROOT);
        } else if (delegated_cgroup_root(derived, sizeof(derived)) == 0 &&
                   mkdir_usable(derived) == 0) {
            snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s", derived);
        } else if (user_manager_cgroup_root(derived, sizeof(derived)) == 0 &&
                   mkdir_usable(derived) == 0) {
            /* WSL/Ubuntu launch interactive shells outside systemd's user
             * subtree (/non-systemd or a root-owned login session scope), but
             * user@<uid>.service is delegated to the user. Managing containers
             * there keeps real kernel enforcement without root. */
            snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s", derived);
        } else {
            /* Keep the documented default: create() will surface the real
             * permission error rather than silently downgrading enforcement. */
            snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s",
                     NEXUS_CGROUP_DEFAULT_ROOT);
        }
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
            /* Existence is not authority: a root-owned cgroup directory must
             * not be reported usable just because it is a directory. */
            if (S_ISDIR(st.st_mode) && access(path, W_OK) == 0)
                return 0;
#endif
        }
        /* EEXIST but not a writable directory (or stat failed): error */
    }
    /* Re-stat: concurrent creator may have made it between mkdir and here. */
    {
        struct stat st;
        if (stat(path, &st) == 0) {
#ifdef _WIN32
            return 0;
#else
            if (S_ISDIR(st.st_mode) && access(path, W_OK) == 0)
                return 0;
#endif
        }
    }
    return -1;
}

#ifndef _WIN32
/* Read this process's unified cgroup path ("0::<path>") from
 * /proc/self/cgroup into `out`. Returns 0 on success. */
static int self_cgroup_rel(char *out, size_t len)
{
    FILE *f;
    int ok = 0;

    if (len == 0)
        return -1;
    f = fopen("/proc/self/cgroup", "r");
    if (f == NULL)
        return -1;
    out[0] = '\0';
    {
        char line[512];
        while (fgets(line, sizeof(line), f) != NULL) {
            if (line[0] == '0' && line[1] == ':' && line[2] == ':') {
                const char *p = line + 3;
                size_t k = strlen(p);
                while (k > 0 && (p[k - 1] == '\n' || p[k - 1] == '\r'))
                    k--;
                if (k >= len)
                    break;
                memcpy(out, p, k);
                out[k] = '\0';
                ok = 1;
                break;
            }
        }
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* 1 when `dir` can host a child cgroup with the memory and cpu controllers,
 * i.e. it is a delegated parent rather than a leaf that holds processes. A
 * probe child is created and removed; controllers materialize only under a
 * parent whose cgroup.subtree_control lists them. */
static int dir_hosts_controllers(const char *dir)
{
    char child[NEXUS_PATH_MAX];
    char f[NEXUS_PATH_MAX + 16];
    int ok;

    if (snprintf(child, sizeof(child), "%s/nexusdb_probe_%ld", dir,
                 (long)getpid()) >= (int)sizeof(child))
        return 0;
    (void)rmdir_compat(child);
    if (mkdir_usable(child) != 0)
        return 0;

    snprintf(f, sizeof(f), "%s/memory.max", child);
    ok = (access(f, W_OK) == 0);
    if (ok) {
        snprintf(f, sizeof(f), "%s/cpu.max", child);
        ok = (access(f, W_OK) == 0);
    }
    (void)rmdir_compat(child);
    return ok;
}

/* Derive "<mount><delegated ancestor>/nexusdb" for this process: walk up from
 * the process's own cgroup to the nearest ancestor that can host a controller-
 * enabled child (e.g. a systemd user manager cgroup), so a runtime launched in
 * a delegated session manages its container cgroups there without root. A
 * `systemd-run --user --scope` leaf itself cannot host controllers, so testing
 * only the leaf is not enough. Returns 0 on success. */
static int delegated_cgroup_root(char *out, size_t len)
{
    char rel[NEXUS_PATH_MAX];
    char cur[NEXUS_PATH_MAX];
    const size_t stop = strlen(NEXUS_CGROUP_MOUNT);

    if (self_cgroup_rel(rel, sizeof(rel)) != 0)
        return -1;
    if (strcmp(rel, "/") == 0 || rel[0] == '\0')
        return -1; /* the mount root is handled by the default path already */
    if (snprintf(cur, sizeof(cur), "%s%s", NEXUS_CGROUP_MOUNT, rel) >=
        (int)sizeof(cur))
        return -1;

    for (;;) {
        int n;
        if (dir_hosts_controllers(cur)) {
            n = snprintf(out, len, "%s/nexusdb", cur);
            return (n < 0 || (size_t)n >= len) ? -1 : 0;
        }
        /* Move to the parent cgroup, stopping at (but not above) the mount. */
        {
            char *slash = strrchr(cur, '/');
            if (slash == NULL || (size_t)(slash - cur) < stop)
                break;
            *slash = '\0';
            if (strlen(cur) <= stop)
                break;
        }
    }
    return -1;
}

/* WSL and several container distros start interactive shells outside the
 * systemd user slice. The user manager itself, however, is delegated to the
 * user and can host controller-enabled children. Probe that path explicitly
 * instead of assuming only the caller's own ancestors are usable. */
static int user_manager_cgroup_root(char *out, size_t len)
{
    char base[NEXUS_PATH_MAX];
    int n;
    uid_t uid = getuid();

    n = snprintf(base, sizeof(base),
                 NEXUS_CGROUP_MOUNT "/user.slice/user-%lu.slice/user@%lu.service",
                 (unsigned long)uid, (unsigned long)uid);
    if (n < 0 || (size_t)n >= sizeof(base))
        return -1;
    if (!dir_hosts_controllers(base))
        return -1;
    n = snprintf(out, len, "%s/nexusdb", base);
    return (n < 0 || (size_t)n >= len) ? -1 : 0;
}

int cgroup_self_in_delegated_root(void)
{
    char root[NEXUS_PATH_MAX];
    char rel[NEXUS_PATH_MAX];
    char abs[NEXUS_PATH_MAX];
    const char *effective = get_root();
    size_t mount_len = strlen(NEXUS_CGROUP_MOUNT);
    size_t root_len;

    if (effective == NULL || effective[0] == '\0')
        return 0;

    /* Explicit shim/test roots are ordinary directories. Kernel migration rules
     * do not apply there; treating them as "already delegated" keeps the
     * library usable in tests without weakening real cgroup enforcement. */
    if (strncmp(effective, NEXUS_CGROUP_MOUNT "/", mount_len + 1) != 0)
        return 1;

    snprintf(root, sizeof(root), "%s", effective);
    root_len = strlen(root);
    while (root_len > 0 && root[root_len - 1] == '/')
        root[--root_len] = '\0';
    {
        const char *slash = strrchr(root, '/');
        if (slash == NULL || strcmp(slash + 1, "nexusdb") != 0)
            return 0;
        root_len = (size_t)(slash - root);
    }

    if (self_cgroup_rel(rel, sizeof(rel)) != 0)
        return 0;
    if (snprintf(abs, sizeof(abs), "%s%s", NEXUS_CGROUP_MOUNT, rel) >=
        (int)sizeof(abs))
        return 0;

    if (strcmp(abs, root) == 0)
        return 1;
    return (strncmp(abs, root, root_len) == 0 && abs[root_len] == '/') ? 1 : 0;
}
#else
int cgroup_self_in_delegated_root(void)
{
    return 1; /* no kernel cgroup delegation on the Windows shim */
}
#endif /* !_WIN32 */

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
    cgroup_clear_error();
    if (root_override && root_override[0] != '\0') {
        if (strlen(root_override) >= sizeof(g_cgroup_root)) {
            set_error_text("cgroup root override is too long");
            return NEXUS_ERR_INVALID;
        }
        snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s", root_override);
    } else {
        (void)get_root();
    }

    if (mkdir_usable(g_cgroup_root) != 0) {
        set_error_errno("create or access cgroup root", g_cgroup_root);
        return NEXUS_ERR_PROCESS;
    }

#ifndef _WIN32
    /* Best-effort: already-enabled controllers or unprivileged hierarchies
     * must not fail init when the root itself is usable. Limit read-back in
     * cgroup_set_limits() is what proves the controllers are actually active. */
    {
        /* g_cgroup_root holds up to NEXUS_PATH_MAX-1 chars; suffixing
         * "/cgroup.subtree_control" needs a buffer one suffix larger so the
         * path can never be silently truncated before being written. */
        char sub_path[NEXUS_PATH_MAX + 32];
        int n = snprintf(sub_path, sizeof(sub_path), "%s/cgroup.subtree_control", g_cgroup_root);
        if (n < 0 || (size_t)n >= sizeof(sub_path)) {
            set_error_text("cgroup subtree_control path is too long");
            return NEXUS_ERR_INVALID;
        }
        (void)write_file_str(sub_path, "+memory +cpu");
    }
#endif

    return NEXUS_OK;
}

int cgroup_create(const char *name, char *out_path, size_t out_path_len)
{
    int n;

    cgroup_clear_error();
    if (!cgroup_name_valid(name) || !out_path || out_path_len == 0) {
        set_error_text("invalid cgroup name or output buffer");
        return NEXUS_ERR_INVALID;
    }

    /* Ensure root exists (EEXIST-tolerant). A missing/uncreatable root is a
     * real failure, not something to silently ignore. */
    {
        const char *root = get_root();
        if (mkdir_usable(root) != 0) {
            set_error_errno("create or access cgroup root", root);
            return NEXUS_ERR_PROCESS;
        }
    }

    {
        const char *root = get_root();
#ifdef _WIN32
        n = snprintf(out_path, out_path_len, "%s\\%s", root, name);
#else
        n = snprintf(out_path, out_path_len, "%s/%s", root, name);
#endif
        if (n < 0 || (size_t)n >= out_path_len) {
            set_error_text("container cgroup path is too long");
            return NEXUS_ERR_INVALID;
        }
    }

    if (mkdir_usable(out_path) != 0) {
        set_error_errno("create container cgroup", out_path);
        return NEXUS_ERR_PROCESS;
    }

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
    char got[64];

    cgroup_clear_error();
    if (!cgroup_path || cgroup_path[0] == '\0' || cpu_percent > 100) {
        set_error_text("invalid cgroup limit argument");
        return NEXUS_ERR_INVALID;
    }

    /* 1. Memory limit -> memory.max */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "memory.max");

    if (memory_bytes > 0)
        snprintf(val, sizeof(val), "%lu", memory_bytes);
    else
        snprintf(val, sizeof(val), "max");
    if (write_file_str(file_path, val) != 0) {
        set_error_errno("write memory.max", file_path);
        return NEXUS_ERR_PROCESS;
    }

    /* Read the effective value back. A write returning success is not enough:
     * a wrong controller, a partial limit, or an unsupported value must not be
     * reported as an enforced limit. */
    {
        FILE *f = fopen(file_path, "r");
        int scanned = f ? fscanf(f, "%63s", got) : -1;
        if (f)
            fclose(f);
        if (scanned != 1) {
            snprintf(g_last_error, sizeof(g_last_error),
                     "cannot read back memory.max at '%s'", file_path);
            return NEXUS_ERR_PROCESS;
        }
        if (memory_bytes > 0) {
            char *end = NULL;
            unsigned long actual;
            errno = 0;
            actual = strtoul(got, &end, 10);
            if (errno != 0 || end == got || *end != '\0' ||
                actual != memory_bytes) {
                snprintf(g_last_error, sizeof(g_last_error),
                         "memory.max at '%s' effective '%s' (requested %lu)",
                         file_path, got, memory_bytes);
                return NEXUS_ERR_PROCESS;
            }
        } else if (strcmp(got, "max") != 0) {
            snprintf(g_last_error, sizeof(g_last_error),
                     "memory.max at '%s' effective '%s' (requested unlimited)",
                     file_path, got);
            return NEXUS_ERR_PROCESS;
        }
    }

    /* 2. CPU limit -> cpu.max (quota, period) */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "cpu.max");

    if (cpu_percent > 0) {
        unsigned long period = 100000;
        unsigned long quota = (period * cpu_percent) / 100;
        snprintf(val, sizeof(val), "%lu %lu", quota, period);
    } else {
        snprintf(val, sizeof(val), "max 100000");
    }
    if (write_file_str(file_path, val) != 0) {
        set_error_errno("write cpu.max", file_path);
        return NEXUS_ERR_PROCESS;
    }

    {
        char quota_tok[64];
        unsigned long period = 0;
        FILE *f = fopen(file_path, "r");
        int scanned = f ? fscanf(f, "%63s %lu", quota_tok, &period) : -1;
        if (f)
            fclose(f);
        if (scanned != 2) {
            snprintf(g_last_error, sizeof(g_last_error),
                     "cannot read back cpu.max at '%s'", file_path);
            return NEXUS_ERR_PROCESS;
        }
        if (cpu_percent > 0) {
            unsigned long expected_period = 100000;
            unsigned long expected_quota = (expected_period * cpu_percent) / 100;
            char *end = NULL;
            unsigned long actual;
            errno = 0;
            actual = strtoul(quota_tok, &end, 10);
            if (errno != 0 || end == quota_tok || *end != '\0' ||
                actual != expected_quota || period != expected_period) {
                snprintf(g_last_error, sizeof(g_last_error),
                         "cpu.max at '%s' effective '%s %lu' (requested %lu %lu)",
                         file_path, quota_tok, period, expected_quota,
                         expected_period);
                return NEXUS_ERR_PROCESS;
            }
        } else if (strcmp(quota_tok, "max") != 0 || period != 100000) {
            snprintf(g_last_error, sizeof(g_last_error),
                     "cpu.max at '%s' effective '%s %lu' (requested max 100000)",
                     file_path, quota_tok, period);
            return NEXUS_ERR_PROCESS;
        }
    }

    return NEXUS_OK;
}

/* 1 when cgroup.procs lists `pid`; 0 when it does not; -1 when the file cannot
 * be read (unavailable is distinct from absent). */
static int procs_contains_pid(const char *cgroup_path, long pid)
{
    char file_path[NEXUS_PATH_MAX + 32];
    FILE *f;

    build_child_path(file_path, sizeof(file_path), cgroup_path, "cgroup.procs");
    f = fopen(file_path, "r");
    if (!f)
        return -1;
    for (;;) {
        long v;
        if (fscanf(f, "%ld", &v) != 1)
            break;
        if (v == pid) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int cgroup_attach(const char *cgroup_path, long pid)
{
    char file_path[NEXUS_PATH_MAX + 32];
    char val[32];

    cgroup_clear_error();
    if (!cgroup_path || cgroup_path[0] == '\0' || pid <= 0) {
        set_error_text("invalid cgroup path or pid");
        return NEXUS_ERR_INVALID;
    }

    build_child_path(file_path, sizeof(file_path), cgroup_path, "cgroup.procs");

    snprintf(val, sizeof(val), "%ld", pid);
    if (write_file_str(file_path, val) != 0) {
        set_error_errno("attach pid to cgroup.procs", file_path);
        return NEXUS_ERR_PROCESS;
    }

    /* Verify the move actually took effect. Without this, an attach that the
     * kernel silently declined (e.g. a process outside the delegated subtree,
     * which needs write access to the migration common ancestor) would look
     * like an enforced limit while the workload runs unconstrained. */
    if (procs_contains_pid(cgroup_path, pid) != 1) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "pid %ld is not in '%s' after attach (migration was denied; "
                 "the process must be inside the delegated cgroup subtree)",
                 pid, file_path);
        return NEXUS_ERR_PROCESS;
    }
    return NEXUS_OK;
}

int cgroup_count_procs(const char *cgroup_path)
{
    char file_path[NEXUS_PATH_MAX + 32];
    FILE *f;
    int count = 0;

    if (!cgroup_path || cgroup_path[0] == '\0')
        return -1;

    build_child_path(file_path, sizeof(file_path), cgroup_path, "cgroup.procs");
    f = fopen(file_path, "r");
    if (!f)
        return -1;
    for (;;) {
        long v;
        if (fscanf(f, "%ld", &v) != 1)
            break;
        if (v > 0)
            count++;
    }
    fclose(f);
    return count;
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

    /* 4. cpu.max (OPTIONAL): "max 100000" or "<quota> <period>", microseconds. */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "cpu.max");
    f = fopen(file_path, "r");
    if (f) {
        char q[32];
        unsigned long period = 0;
        int got = fscanf(f, "%31s %lu", q, &period);
        if (got >= 1) {
            if (strcmp(q, "max") == 0) {
                metrics->has_cpu_limit = 0;   /* unlimited */
                metrics->cpu_quota_us = 0;
            } else {
                char *end = NULL;
                unsigned long v;
                errno = 0;
                v = strtoul(q, &end, 10);
                if (errno == 0 && end != q && *end == '\0') {
                    metrics->has_cpu_limit = (v > 0);
                    metrics->cpu_quota_us = v;
                }
            }
            if (got == 2)
                metrics->cpu_period_us = period;
        }
        fclose(f);
    }

    /* 5. memory.pressure (OPTIONAL, PSI): capture "some avg10" as a percent. */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "memory.pressure");
    f = fopen(file_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            if (strncmp(line, "some", 4) == 0) {
                char *p = strstr(line, "avg10=");
                if (p != NULL) {
                    char *end = NULL;
                    double value;
                    errno = 0;
                    value = strtod(p + 6, &end);
                    if (errno == 0 && end != p + 6) {
                        metrics->pressure_some_avg10 = value;
                        metrics->has_pressure = 1;
                    }
                }
                break;
            }
        }
        fclose(f);
    }

    /* 6. memory.events (OPTIONAL): OOM counters. */
    build_child_path(file_path, sizeof(file_path), cgroup_path, "memory.events");
    f = fopen(file_path, "r");
    if (f) {
        char key[64];
        unsigned long val = 0;
        while (fscanf(f, "%63s %lu", key, &val) == 2) {
            if (strcmp(key, "oom") == 0)
                metrics->memory_oom_events = val;
            else if (strcmp(key, "oom_kill") == 0)
                metrics->memory_oom_kill_events = val;
        }
        metrics->has_memory_events = 1;
        fclose(f);
    }

    /* 7. Calculate memory pressure */
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

    cgroup_clear_error();
    if (!cgroup_path || cgroup_path[0] == '\0') {
        set_error_text("invalid cgroup path");
        return NEXUS_ERR_INVALID;
    }

    /* Never rmdir a cgroup that still owns live processes. Stale dead-pid
     * entries are drained best-effort inside procs_has_live(). */
    if (procs_has_live(cgroup_path)) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "cgroup '%s' still contains live processes", cgroup_path);
        return NEXUS_ERR_STATE;
    }

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
        set_error_errno("remove cgroup", cgroup_path);
        return NEXUS_ERR_PROCESS;
    }
    return NEXUS_OK;
}
