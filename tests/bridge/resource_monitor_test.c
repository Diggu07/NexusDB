/*
 * NexusDB — OS–DB Bridge resource-monitor test (Rehan).
 *
 * Builds isolated cgroup-directory fixtures under the OS temp dir (no root,
 * no real cgroup required) and checks that the bridge reports real values and
 * honest availability:
 *   - valid readings (memory current/max, pressure, cpu usage)
 *   - unlimited ("max") memory
 *   - missing cpu.stat -> cpu unavailable, memory still valid
 *   - malformed / missing required memory files -> real error, no fabrication
 *   - CPU-rate math, counter reset, zero interval, absent CPU data
 *   - container-name lookup via the container runtime
 *
 * Exit 0 on success, 1 on any failure. Same PASS/FAIL style as the other
 * tests in this repository.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid_compat() _getpid()
#define mkdir_compat(p) _mkdir(p)
#define rmdir_compat(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define getpid_compat() (int)getpid()
#define mkdir_compat(p) mkdir((p), 0755)
#define rmdir_compat(p) rmdir(p)
#endif

#include "bridge/resource_monitor.h"
#include "container/container.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

static void join(char *buf, size_t len, const char *dir, const char *file)
{
#ifdef _WIN32
    snprintf(buf, len, "%s\\%s", dir, file);
#else
    snprintf(buf, len, "%s/%s", dir, file);
#endif
}

static void test_root(char *buf, size_t len)
{
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
#ifdef _WIN32
    if (!tmp) tmp = ".";
    snprintf(buf, len, "%s\\nexus_bridge_test_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#else
    if (!tmp) tmp = "/tmp";
    snprintf(buf, len, "%s/nexus_bridge_test_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#endif
}

/* Create a fresh sub-directory fixture and copy its path into `out`. */
static void make_case(const char *root, const char *name, char *out, size_t outlen)
{
    join(out, outlen, root, name);
    (void)mkdir_compat(out);
}

static int write_case(const char *dir, const char *file, const char *content)
{
    char path[NEXUS_PATH_MAX + 32];
    FILE *f;
    join(path, sizeof(path), dir, file);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(content, f);
    if (fclose(f) != 0)
        return -1;
    return 0;
}

static void remove_case(const char *dir, const char *file)
{
    char path[NEXUS_PATH_MAX + 32];
    join(path, sizeof(path), dir, file);
    (void)remove(path);
}

int main(void)
{
    char root[NEXUS_PATH_MAX];
    char valid_dir[NEXUS_PATH_MAX];
    char nocpu_dir[NEXUS_PATH_MAX];
    char unlim_dir[NEXUS_PATH_MAX];
    char badmax_dir[NEXUS_PATH_MAX];
    char badcur_dir[NEXUS_PATH_MAX];
    char nomax_dir[NEXUS_PATH_MAX];
    char nocur_dir[NEXUS_PATH_MAX];
    char cpua_dir[NEXUS_PATH_MAX];
    char cpub_dir[NEXUS_PATH_MAX];
    char missing[NEXUS_PATH_MAX];
    nexus_resource_snapshot_t s, prev, cur;
    double pct;

    printf("=== NexusDB OS-DB Bridge: resource monitor test ===\n\n");

    test_root(root, sizeof(root));
    (void)mkdir_compat(root);
    printf("fixture root: %s\n\n", root);

    /* ---- argument validation ---- */
    CHECK("read_cgroup NULL dir -> INVALID",
          nexus_bridge_read_cgroup(NULL, &s) == NEXUS_ERR_INVALID);
    CHECK("read_cgroup empty dir -> INVALID",
          nexus_bridge_read_cgroup("", &s) == NEXUS_ERR_INVALID);
    CHECK("read_cgroup NULL out -> INVALID",
          nexus_bridge_read_cgroup(root, NULL) == NEXUS_ERR_INVALID);

    join(missing, sizeof(missing), root, "does-not-exist");
    memset(&s, 0xAB, sizeof(s));
    CHECK("read_cgroup missing dir -> PROCESS",
          nexus_bridge_read_cgroup(missing, &s) == NEXUS_ERR_PROCESS);
    CHECK("failed read leaves snapshot invalid (no fabrication)", s.valid == 0);

    /* ---- valid fixture ---- */
    make_case(root, "valid", valid_dir, sizeof(valid_dir));
    (void)write_case(valid_dir, "memory.current", "1048576");
    (void)write_case(valid_dir, "memory.max", "4194304");
    (void)write_case(valid_dir, "cpu.stat", "usage_usec 12345\n");
    {
        int rc = nexus_bridge_read_cgroup(valid_dir, &s);
        printf("--- valid: rc=%s current=%lu max=%lu pressure=%.4f cpu=%lu ---\n",
               nexus_bridge_strerror(rc), s.memory_current, s.memory_max,
               s.memory_pressure, s.cpu_usage_usec);
        CHECK("valid fixture -> OK", rc == NEXUS_OK);
        CHECK("valid snapshot valid", s.valid == 1);
        CHECK("has_memory set", s.has_memory == 1);
        CHECK("has_limit set", s.has_limit == 1);
        CHECK("memory_current read", s.memory_current == 1048576UL);
        CHECK("memory_max read", s.memory_max == 4194304UL);
        {
            double diff = s.memory_pressure - 0.25;
            if (diff < 0) diff = -diff;
            CHECK("pressure == current/max (0.25)", diff < 1e-9);
        }
        CHECK("has_cpu set when cpu.stat present", s.has_cpu == 1);
        CHECK("cpu_usage_usec read", s.cpu_usage_usec == 12345UL);
    }

    /* ---- missing cpu.stat: memory valid, cpu explicitly unavailable ---- */
    make_case(root, "nocpu", nocpu_dir, sizeof(nocpu_dir));
    (void)write_case(nocpu_dir, "memory.current", "2048");
    (void)write_case(nocpu_dir, "memory.max", "8192");
    {
        int rc = nexus_bridge_read_cgroup(nocpu_dir, &s);
        CHECK("no cpu.stat -> still OK", rc == NEXUS_OK);
        CHECK("no cpu.stat -> snapshot valid", s.valid == 1);
        CHECK("no cpu.stat -> has_cpu == 0", s.has_cpu == 0);
        CHECK("no cpu.stat -> cpu_usage_usec == 0", s.cpu_usage_usec == 0);
    }

    /* ---- unlimited ("max") memory ---- */
    make_case(root, "unlim", unlim_dir, sizeof(unlim_dir));
    (void)write_case(unlim_dir, "memory.current", "4096");
    (void)write_case(unlim_dir, "memory.max", "max");
    {
        int rc = nexus_bridge_read_cgroup(unlim_dir, &s);
        CHECK("unlimited -> OK", rc == NEXUS_OK);
        CHECK("unlimited -> valid", s.valid == 1);
        CHECK("unlimited -> has_limit == 0", s.has_limit == 0);
        CHECK("unlimited -> memory_max == 0", s.memory_max == 0);
        CHECK("unlimited -> pressure == 0.0", s.memory_pressure == 0.0);
    }

    /* ---- malformed / missing required files ---- */
    make_case(root, "badmax", badmax_dir, sizeof(badmax_dir));
    (void)write_case(badmax_dir, "memory.current", "1000");
    (void)write_case(badmax_dir, "memory.max", "not-a-number");
    memset(&s, 0xAB, sizeof(s));
    CHECK("malformed memory.max -> PROCESS",
          nexus_bridge_read_cgroup(badmax_dir, &s) == NEXUS_ERR_PROCESS);
    CHECK("malformed memory.max -> invalid snapshot", s.valid == 0);

    make_case(root, "badcur", badcur_dir, sizeof(badcur_dir));
    (void)write_case(badcur_dir, "memory.current", "not-a-number");
    (void)write_case(badcur_dir, "memory.max", "1000");
    CHECK("malformed memory.current -> PROCESS",
          nexus_bridge_read_cgroup(badcur_dir, &s) == NEXUS_ERR_PROCESS);

    make_case(root, "nomax", nomax_dir, sizeof(nomax_dir));
    (void)write_case(nomax_dir, "memory.current", "1000");
    CHECK("missing memory.max -> PROCESS",
          nexus_bridge_read_cgroup(nomax_dir, &s) == NEXUS_ERR_PROCESS);

    make_case(root, "nocur", nocur_dir, sizeof(nocur_dir));
    (void)write_case(nocur_dir, "memory.max", "1000");
    CHECK("missing memory.current -> PROCESS",
          nexus_bridge_read_cgroup(nocur_dir, &s) == NEXUS_ERR_PROCESS);

    /* ---- CPU percent math ---- */
    make_case(root, "cpua", cpua_dir, sizeof(cpua_dir));
    (void)write_case(cpua_dir, "memory.current", "1000");
    (void)write_case(cpua_dir, "memory.max", "10000");
    (void)write_case(cpua_dir, "cpu.stat", "usage_usec 1000000\n");
    make_case(root, "cpub", cpub_dir, sizeof(cpub_dir));
    (void)write_case(cpub_dir, "memory.current", "1000");
    (void)write_case(cpub_dir, "memory.max", "10000");
    (void)write_case(cpub_dir, "cpu.stat", "usage_usec 1500000\n");

    CHECK("cpu%: prev read OK",
          nexus_bridge_read_cgroup(cpua_dir, &prev) == NEXUS_OK);
    CHECK("cpu%: cur read OK",
          nexus_bridge_read_cgroup(cpub_dir, &cur) == NEXUS_OK);
    pct = nexus_bridge_cpu_percent(&prev, &cur, 1000);
    printf("--- cpu%%: delta=500000usec over 1000ms -> %.2f%% ---\n", pct);
    {
        double diff = pct - 50.0;
        if (diff < 0) diff = -diff;
        CHECK("cpu%: 50.0 for half a core-second per second", diff < 1e-9);
    }
    CHECK("cpu%: zero interval -> negative",
          nexus_bridge_cpu_percent(&prev, &cur, 0) < 0.0);
    CHECK("cpu%: backwards counter (reset) -> negative",
          nexus_bridge_cpu_percent(&cur, &prev, 1000) < 0.0);
    CHECK("cpu%: NULL -> negative",
          nexus_bridge_cpu_percent(NULL, &cur, 1000) < 0.0);

    /* prev without cpu.stat must refuse a rate. */
    {
        nexus_resource_snapshot_t nocpu;
        CHECK("cpu%: snapshot without cpu data -> negative",
              (nexus_bridge_read_cgroup(nocpu_dir, &nocpu) == NEXUS_OK) &&
                  nexus_bridge_cpu_percent(&nocpu, &cur, 1000) < 0.0);
    }

    /* ---- container lookup via the runtime ---- */
    CHECK("container wrapper NULL name -> INVALID",
          nexus_bridge_read_container(NULL, &s) == NEXUS_ERR_INVALID);
    CHECK("container wrapper unknown -> NOT_FOUND",
          nexus_bridge_read_container("nope-not-created", &s) == NEXUS_ERR_NOT_FOUND);
    {
        int rc = container_create("brtest1", 268435456UL, 50, "./db_engine");
        CHECK("container_create for bridge lookup", rc == NEXUS_OK);
        rc = nexus_bridge_read_container("brtest1", &s);
        CHECK("container wrapper reads real cgroup path", rc == NEXUS_OK);
        CHECK("container wrapper snapshot valid", s.valid == 1);
        CHECK("container wrapper has_limit", s.has_limit == 1);
        CHECK("container wrapper memory_max == configured limit",
              s.memory_max == 268435456UL);
        (void)container_delete("brtest1");
    }

    /* ---- misc ---- */
    CHECK("strerror never NULL", nexus_bridge_strerror(NEXUS_ERR_PROCESS) != NULL);

    /* ---- cleanup (best effort) ---- */
    remove_case(valid_dir, "memory.current");
    remove_case(valid_dir, "memory.max");
    remove_case(valid_dir, "cpu.stat");
    remove_case(nocpu_dir, "memory.current");
    remove_case(nocpu_dir, "memory.max");
    remove_case(unlim_dir, "memory.current");
    remove_case(unlim_dir, "memory.max");
    remove_case(badmax_dir, "memory.current");
    remove_case(badmax_dir, "memory.max");
    remove_case(badcur_dir, "memory.current");
    remove_case(badcur_dir, "memory.max");
    remove_case(nomax_dir, "memory.current");
    remove_case(nocur_dir, "memory.max");
    remove_case(cpua_dir, "memory.current");
    remove_case(cpua_dir, "memory.max");
    remove_case(cpua_dir, "cpu.stat");
    remove_case(cpub_dir, "memory.current");
    remove_case(cpub_dir, "memory.max");
    remove_case(cpub_dir, "cpu.stat");
    (void)rmdir_compat(valid_dir);
    (void)rmdir_compat(nocpu_dir);
    (void)rmdir_compat(unlim_dir);
    (void)rmdir_compat(badmax_dir);
    (void)rmdir_compat(badcur_dir);
    (void)rmdir_compat(nomax_dir);
    (void)rmdir_compat(nocur_dir);
    (void)rmdir_compat(cpua_dir);
    (void)rmdir_compat(cpub_dir);
    (void)rmdir_compat(root);

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
