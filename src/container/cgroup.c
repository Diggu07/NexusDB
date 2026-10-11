/*
 * NexusDB — Container Engine: cgroups v2 implementation (Ayush: Systems Engineer)
 *
 * Provides resource isolation and real-time observability.
 * Real target: Ubuntu 22.04 / WSL2 with cgroups v2.
 * Dev target: Windows fallback shim with mock files / graceful handling.
 */
#include "container/cgroup.h"
#include "container/container.h"

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

static int write_file_str(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    if (fprintf(f, "%s\n", value) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int cgroup_init(const char *root_override)
{
    if (root_override && root_override[0] != '\0') {
        snprintf(g_cgroup_root, sizeof(g_cgroup_root), "%s", root_override);
    } else {
        (void)get_root();
    }

    (void)mkdir_compat(g_cgroup_root);

#ifndef _WIN32
    /* Enable controllers for child groups if running under cgroup v2 hierarchy */
    char sub_path[NEXUS_PATH_MAX];
    snprintf(sub_path, sizeof(sub_path), "%s/cgroup.subtree_control", g_cgroup_root);
    (void)write_file_str(sub_path, "+memory +cpu");
#endif

    return NEXUS_OK;
}

int cgroup_create(const char *name, char *out_path, size_t out_path_len)
{
    if (!name || !out_path || out_path_len == 0)
        return NEXUS_ERR_INVALID;

    const char *root = get_root();
#ifdef _WIN32
    snprintf(out_path, out_path_len, "%s\\%s", root, name);
#else
    snprintf(out_path, out_path_len, "%s/%s", root, name);
#endif

    (void)mkdir_compat(root);
    (void)mkdir_compat(out_path);
    return NEXUS_OK;
}

int cgroup_set_limits(const char *cgroup_path, unsigned long memory_bytes, unsigned int cpu_percent)
{
    if (!cgroup_path || cgroup_path[0] == '\0')
        return NEXUS_ERR_INVALID;

    char file_path[NEXUS_PATH_MAX + 32];
    char val[64];

    /* 1. Memory limit -> memory.max */
#ifdef _WIN32
    snprintf(file_path, sizeof(file_path), "%s\\memory.max", cgroup_path);
#else
    snprintf(file_path, sizeof(file_path), "%s/memory.max", cgroup_path);
#endif

    if (memory_bytes > 0)
        snprintf(val, sizeof(val), "%lu", memory_bytes);
    else
        snprintf(val, sizeof(val), "max");
    (void)write_file_str(file_path, val);

    /* 2. CPU limit -> cpu.max (quota, period) */
#ifdef _WIN32
    snprintf(file_path, sizeof(file_path), "%s\\cpu.max", cgroup_path);
#else
    snprintf(file_path, sizeof(file_path), "%s/cpu.max", cgroup_path);
#endif

    if (cpu_percent > 0 && cpu_percent <= 100) {
        unsigned long period = 100000;
        unsigned long quota = (period * cpu_percent) / 100;
        snprintf(val, sizeof(val), "%lu %lu", quota, period);
    } else {
        snprintf(val, sizeof(val), "max 100000");
    }
    (void)write_file_str(file_path, val);

    return NEXUS_OK;
}

int cgroup_attach(const char *cgroup_path, long pid)
{
    if (!cgroup_path || pid <= 0)
        return NEXUS_ERR_INVALID;

    char file_path[NEXUS_PATH_MAX + 32];
    char val[32];

#ifdef _WIN32
    snprintf(file_path, sizeof(file_path), "%s\\cgroup.procs", cgroup_path);
#else
    snprintf(file_path, sizeof(file_path), "%s/cgroup.procs", cgroup_path);
#endif

    snprintf(val, sizeof(val), "%ld", pid);
    (void)write_file_str(file_path, val);
    return NEXUS_OK;
}

int cgroup_read_metrics(const char *cgroup_path, nexus_cgroup_metrics_t *metrics)
{
    if (!cgroup_path || !metrics)
        return NEXUS_ERR_INVALID;

    memset(metrics, 0, sizeof(*metrics));

    char file_path[NEXUS_PATH_MAX + 32];
    FILE *f;

    /* 1. Read memory.current */
#ifdef _WIN32
    snprintf(file_path, sizeof(file_path), "%s\\memory.current", cgroup_path);
#else
    snprintf(file_path, sizeof(file_path), "%s/memory.current", cgroup_path);
#endif
    f = fopen(file_path, "r");
    if (f) {
        if (fscanf(f, "%lu", &metrics->memory_current) != 1)
            metrics->memory_current = 0;
        fclose(f);
    }

    /* 2. Read memory.max */
#ifdef _WIN32
    snprintf(file_path, sizeof(file_path), "%s\\memory.max", cgroup_path);
#else
    snprintf(file_path, sizeof(file_path), "%s/memory.max", cgroup_path);
#endif
    f = fopen(file_path, "r");
    if (f) {
        char buf[32];
        if (fscanf(f, "%31s", buf) == 1) {
            if (strcmp(buf, "max") != 0)
                metrics->memory_max = strtoul(buf, NULL, 10);
            else
                metrics->memory_max = 0;
        }
        fclose(f);
    }

    /* 3. Read cpu.stat (usage_usec) */
#ifdef _WIN32
    snprintf(file_path, sizeof(file_path), "%s\\cpu.stat", cgroup_path);
#else
    snprintf(file_path, sizeof(file_path), "%s/cpu.stat", cgroup_path);
#endif
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
    }

    /* 4. Calculate memory pressure */
    if (metrics->memory_max > 0) {
        metrics->memory_pressure = (double)metrics->memory_current / (double)metrics->memory_max;
    } else {
        metrics->memory_pressure = 0.0;
    }

    return NEXUS_OK;
}

int cgroup_destroy(const char *cgroup_path)
{
    if (!cgroup_path || cgroup_path[0] == '\0')
        return NEXUS_ERR_INVALID;

    /* Remove mock files if any */
    char file_path[NEXUS_PATH_MAX + 32];
    const char *files[] = {"memory.max", "memory.current", "cpu.max", "cpu.stat", "cgroup.procs", NULL};
    for (int i = 0; files[i] != NULL; ++i) {
#ifdef _WIN32
        snprintf(file_path, sizeof(file_path), "%s\\%s", cgroup_path, files[i]);
#else
        snprintf(file_path, sizeof(file_path), "%s/%s", cgroup_path, files[i]);
#endif
        (void)remove(file_path);
    }

    (void)rmdir_compat(cgroup_path);
    return NEXUS_OK;
}
