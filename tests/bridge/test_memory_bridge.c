/*
 * NexusDB — OS-to-DB Bridge: memory-telemetry relay test.
 *
 * Isolated milestone test (no sudo, NEXUS_CGROUP_ROOT override). Exercises
 * ONLY the new bridge module through the existing container interface:
 *   - argument validation (NULL/small-buffer handling, no crashes)
 *   - query round-trip against a real container (values match the limits
 *     configured at create; the bridge performs no second allocation)
 *   - display formatting for the CLI/demo output style
 *   - read-only pressure-hint classification (no action taken)
 *   - bridge purity: querying never modifies the configured limits
 *   - unknown-container and post-delete behaviour
 *
 * Exit 0 on success, 1 on any failure. Same PASS/FAIL style as the
 * container suites. Does not touch Rehan's future policy/dashboard code.
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
#include <unistd.h>
#define getpid_compat() (int)getpid()
#endif

#include "bridge/memory_bridge.h"
#include "container/cgroup.h"
#include "container/container.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

/* Build an isolated, per-run cgroup root under the OS temp dir. */
static void test_root(char *buf, size_t len)
{
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
#ifdef _WIN32
    if (!tmp) tmp = ".";
    snprintf(buf, len, "%s\\nexus_mb-test_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#else
    if (!tmp) tmp = "/tmp";
    snprintf(buf, len, "%s/nexus_mb-test_%d_%ld",
             tmp, getpid_compat(), (long)time(NULL));
#endif
}

int main(void)
{
    char root[NEXUS_PATH_MAX];
    char line[256];
    int rc;
    nexus_cgroup_metrics_t m, before, after;

    printf("=== NexusDB memory-telemetry bridge test (isolated root) ===\n\n");

    test_root(root, sizeof(root));
    printf("isolated root: %s\n", root);
    rc = cgroup_init(root);
    CHECK("cgroup_init isolated root", rc == NEXUS_OK);

    /* 1. Validation: NULL args rejected, small buffers rejected. */
    CHECK("query NULL out -> INVALID",
          memory_bridge_query("anything", NULL) == NEXUS_ERR_INVALID);
    CHECK("query NULL name -> INVALID",
          memory_bridge_query(NULL, &m) == NEXUS_ERR_INVALID);
    CHECK("format NULL metrics -> INVALID",
          memory_bridge_format(NULL, line, sizeof(line)) == NEXUS_ERR_INVALID);
    CHECK("format NULL buf -> INVALID",
          memory_bridge_format(&m, NULL, sizeof(line)) == NEXUS_ERR_INVALID);
    CHECK("format zero buflen -> INVALID",
          memory_bridge_format(&m, line, 0) == NEXUS_ERR_INVALID);
    {
        char tiny[8];
        CHECK("format tiny buf -> INVALID",
              memory_bridge_format(&m, tiny, sizeof(tiny)) == NEXUS_ERR_INVALID);
    }
    CHECK("pressure_state NULL -> INVALID",
          memory_bridge_pressure_state(NULL) == MEMORY_BRIDGE_INVALID);

    /* 2. Unknown container surfaces the container layer code (no fake data). */
    CHECK("query unknown -> NOT_FOUND",
          memory_bridge_query("ghost_mb", &m) == NEXUS_ERR_NOT_FOUND);

    /* 3. Round-trip against a real container: bridge reports the limits
     * configured at create (256MB / cpu 50) without allocating anything. */
    rc = container_create("mbr1", 256UL * 1024 * 1024, 50, NULL);
    CHECK("create mbr1 (256MB, cpu 50)", rc == NEXUS_OK);
    rc = memory_bridge_query("mbr1", &m);
    CHECK("bridge query mbr1", rc == NEXUS_OK);
    CHECK("bridge reports configured max",
          m.memory_max == 256UL * 1024 * 1024);
    rc = memory_bridge_format(&m, line, sizeof(line));
    CHECK("bridge format mbr1", rc == NEXUS_OK);
    printf("--- bridge display ---\n%s\n------------------------\n", line);
    CHECK("display mentions current",
          strstr(line, "Memory current:") != NULL);
    CHECK("fresh container hints LOW (pressure 0.0, no action)",
          memory_bridge_pressure_state(&m) == MEMORY_BRIDGE_LOW);

    /* 4. Bridge purity: querying twice never modifies the stored limits. */
    before = m;
    rc = memory_bridge_query("mbr1", &after);
    CHECK("second query succeeds", rc == NEXUS_OK);
    CHECK("limits untouched by bridge reads",
          after.memory_max == before.memory_max &&
          after.memory_current == before.memory_current);

    /* 5. Unlimited container formats honestly (no fake zero pressure). */
    rc = container_create("mbrmax", 0, 0, NULL);
    CHECK("create mbrmax (unlimited)", rc == NEXUS_OK);
    rc = memory_bridge_query("mbrmax", &m);
    CHECK("bridge query unlimited", rc == NEXUS_OK && m.memory_max == 0);
    rc = memory_bridge_format(&m, line, sizeof(line));
    CHECK("bridge format unlimited",
          rc == NEXUS_OK && strstr(line, "unlimited") != NULL);
    printf("--- bridge display (unlimited) ---\n%s\n------------------------------------\n", line);
    CHECK("unlimited hints NORMAL",
          memory_bridge_pressure_state(&m) == MEMORY_BRIDGE_NORMAL);
    rc = container_delete("mbrmax");
    CHECK("delete mbrmax", rc == NEXUS_OK);

    /* 6. Post-delete: bridge reports the container layer state, no stale data. */
    rc = container_delete("mbr1");
    CHECK("delete mbr1", rc == NEXUS_OK);
    CHECK("query deleted -> STATE (no stale telemetry)",
          memory_bridge_query("mbr1", &m) == NEXUS_ERR_STATE);

#ifdef _WIN32
    (void)_rmdir(root);
#else
    (void)rmdir(root);
#endif

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
