/*
 * NexusDB — Container Engine lifecycle test/demo.
 *
 * Exercises: create -> start -> status -> stop -> delete,
 * plus error paths (duplicate, bad name, bad limits, illegal states).
 *
 * Exit 0 on success, 1 on any failure. Prints PASS/FAIL per check so the
 * demo doubles as a readable walkthrough.
 */
#include <stdio.h>
#include <string.h>

#include "container/container.h"

static int g_failures = 0;

#define CHECK(desc, expr) do { \
    if (expr) { printf("PASS: %s\n", desc); } \
    else { printf("FAIL: %s\n", desc); g_failures++; } \
} while (0)

int main(void)
{
    char buf[1024];
    int rc;
    const nexus_container_t *c;

    printf("=== NexusDB Container Engine: lifecycle test ===\n\n");

    /* 0. Validation: bad names / bad limits are rejected. */
    rc = container_create("", 0, 0, NULL);
    CHECK("reject empty name", rc == NEXUS_ERR_INVALID);
    rc = container_create("bad name!", 0, 0, NULL);
    CHECK("reject name with space/!", rc == NEXUS_ERR_INVALID);
    rc = container_create("okname", 0, 101, NULL);
    CHECK("reject cpu > 100", rc == NEXUS_ERR_INVALID);

    /* 1. CREATE */
    rc = container_create("db1", 256UL * 1024 * 1024, 50, "./db_engine");
    CHECK("create db1 (256MB, cpu 50)", rc == NEXUS_OK);
    c = container_get("db1");
    CHECK("registry holds db1 in CREATED with no pid",
          c != NULL && c->state == NEXUS_CREATED && c->pid == -1);

    /* 2. Duplicate create must fail, not corrupt state. */
    rc = container_create("db1", 0, 0, NULL);
    CHECK("duplicate create rejected (EXISTS)", rc == NEXUS_ERR_EXISTS);

    /* 3. STATUS while created. */
    rc = container_status("db1", buf, sizeof(buf));
    CHECK("status db1 works while CREATED", rc == NEXUS_OK);
    printf("--- status (CREATED) ---\n%s--------------------------\n", buf);

    /* 4. START (spawns placeholder child). */
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

    /* 9. Unknown names. */
    rc = container_start("ghost");
    CHECK("start unknown -> NOT_FOUND", rc == NEXUS_ERR_NOT_FOUND);
    rc = container_delete("ghost");
    CHECK("delete unknown -> NOT_FOUND", rc == NEXUS_ERR_NOT_FOUND);

    printf("\n=== %s (%d failure(s)) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
