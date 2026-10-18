# NexusDB — primary build (CMake).
#
# Mirrors the root Makefile: builds the C container runtime + lifecycle
# test from the same sources. C++ DB engine (Digvijay) and Bridge
# (Rehan) targets get added here as their sources land.
#
# Linux (Ubuntu/WSL2, the real target):
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
# Windows dev machine (lifecycle shim only):
#   cmake -S . -B build -G "MinGW Makefiles" && cmake --build build && ctest --test-dir build

cmake_minimum_required(VERSION 3.16)

project(NexusDB LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Werror)
endif()

include_directories(include)

add_executable(nexus_container_test
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  tests/container/container_test.c
)

# Runnable demonstration of the Container Engine public API (Ayush's runtime):
# validation, create/start/stop/delete, and error paths. Links the real
# container + placeholder-process sources; the integrated lifecycle provisions
# cgroups in container_create/start/delete, so cgroup.c is linked too. No
# lifecycle logic is reimplemented here.
add_executable(container_engine_demo
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  examples/container/container_engine_demo.c
)

# Part 1: isolated cgroup reliability test (NEXUS_CGROUP_ROOT, no sudo).
add_executable(nexus_cgroup_test
  src/container/cgroup.c
  src/container/process.c
  tests/container/cgroup_test.c
)

# P0: direct process-execution API test (POSIX; Windows reports ENOSYS).
add_executable(nexus_process_test
  src/container/process.c
  tests/container/process_test.c
)

# P2: Linux namespace integration test. Isolation checks are inode-based
# (/proc/<pid>/ns) and require CAP_SYS_ADMIN; when unavailable they are reported
# ENV-LIMITED rather than passed. The denial/failure path is always asserted.
add_executable(nexus_namespace_test
  src/container/process.c
  src/container/namespace.c
  tests/container/namespace_test.c
)

# Phase 1: OS-DB Bridge resource monitoring (Rehan). Reuses the container
# cgroup controller + runtime to read real cgroup v2 statistics and exposes
# explicit per-field availability (no fabricated values). namespace.c is linked
# because container.c (P2) drives the gated namespaced child.
add_executable(nexus_bridge_test
  src/bridge/resource_monitor.c
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  tests/bridge/resource_monitor_test.c
)

# Phase 2: memory-pressure adaptation policy (Rehan). Pure C policy + unit test
# with a fake resize sink (no database or root required).
add_executable(nexus_policy_test
  src/bridge/adaptation_policy.c
  tests/bridge/adaptation_policy_test.c
)

# Phase 2: C++ adapter that routes policy resize requests through the engine's
# public BufferPool API. Built as a static library so it is type-checked
# against the engine without forcing a link here.
add_library(nexus_bridge_adapter STATIC
  src/bridge/buffer_pool_adapter.cpp
)

# Phase 3: live dashboard metrics core + plain-text renderer (Rehan). The
# default build needs no ncurses; Unicode/terminal concerns live in the UI.
add_executable(nexus_dashboard_test
  src/dashboard/metrics.c
  src/dashboard/dashboard_text.c
  src/bridge/adaptation_policy.c
  tests/dashboard/dashboard_test.c
)

add_executable(nexus_dashboard_demo
  src/dashboard/metrics.c
  src/dashboard/dashboard_text.c
  src/bridge/resource_monitor.c
  src/bridge/adaptation_policy.c
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  examples/dashboard/dashboard_demo.c
)

# Optional live ncurses UI: built only when Curses is found (Linux/WSL2).
find_package(Curses)
if(CURSES_FOUND)
  add_executable(nexus_dashboard
    src/dashboard/metrics.c
    src/dashboard/dashboard_text.c
    src/dashboard/dashboard_ncurses.c
    src/bridge/resource_monitor.c
    src/bridge/adaptation_policy.c
    src/container/container.c
    src/container/cgroup.c
    src/container/namespace.c
    src/container/process.c
    examples/dashboard/dashboard_demo.c
  )
  target_compile_definitions(nexus_dashboard PRIVATE NEXUS_HAVE_NCURSES)
  target_include_directories(nexus_dashboard PRIVATE ${CURSES_INCLUDE_DIRS})
  target_link_libraries(nexus_dashboard PRIVATE ${CURSES_LIBRARIES})
endif()
# Phase 1: Page + PageStorage (Digvijay, Database Engine).
add_executable(db_storage_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  tests/db/storage_test.cpp
)

# R1.1: slotted-page record storage (Digvijay, Database Engine). A
# self-validating view over one fixed Page that packs variable-length records
# and keeps stable slot ids; the primitive the record/CRUD layer builds on.
add_executable(slotted_page_test
  src/db/storage/page.cpp
  src/db/record/slotted_page.cpp
  tests/db/slotted_page_test.cpp
)

# R1.2: heap table (Digvijay, Database Engine). Stores variable-length records
# across many pages via SlottedPage + BufferPool, with a recoverable on-disk
# directory chain. Additive over the storage/buffer-pool stack.
add_executable(heap_table_test
  src/db/storage/page.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  tests/db/heap_table_test.cpp
)

# R1.3: persistent catalog (Digvijay, Database Engine). Maps table names to
# HeapTable root page ids in a validated, versioned, chain-linked page format.
add_executable(catalog_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/catalog/catalog.cpp
  tests/db/catalog_test.cpp
)

# R1.3: minimal query/table-access executor (Digvijay, Database Engine).
# Composes Catalog + HeapTable for table-scoped CRUD and scans.
add_executable(query_executor_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/catalog/catalog.cpp
  src/db/query/query_executor.cpp
  tests/db/query_executor_test.cpp
)

# R2: write-ahead log (Digvijay, Database Engine). Append-only page-image redo
# log with LSN ordering and CRC-32 integrity; unit-tested over real files.
add_executable(wal_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/wal/wal.cpp
  tests/db/wal_test.cpp
)

# R2: crash recovery + write-ahead ordering (Digvijay, Database Engine).
# Exercises the BufferPool log barrier and the Database open/recovery path,
# including abrupt child-process termination.
add_executable(recovery_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/catalog/catalog.cpp
  src/db/query/query_executor.cpp
  src/db/wal/wal.cpp
  src/db/database/database.cpp
  tests/db/recovery_test.cpp
)

# Phase 2: BufferPool over PageStorage (Digvijay, Database Engine).
add_executable(buffer_pool_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  tests/db/buffer_pool_test.cpp
)

# Runnable demonstration of the DB engine storage stack (Digvijay): caching,
# LRU eviction, runtime resizing, and dirty-page persistence. Uses only the
# public API, so it links the same engine sources as the buffer-pool test.
add_executable(buffer_pool_demo
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  examples/db/buffer_pool_demo.cpp
)

# Comprehensive Database Engine demonstration (Digvijay): storage, caching,
# LRU, resizing, pinning, dirty write-back, and error handling in one binary.
add_executable(nexusdb_engine_demo
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  examples/db/nexusdb_engine_demo.cpp
)

# P3/R3: the real DB-engine executable the container launches as its workload.
# Minimal and honest: it opens the full Database (storage + WAL + buffer pool +
# catalog/query executor), running WAL recovery on startup, stays alive until
# SIGTERM/SIGINT, then flushes (WAL-ordered) and closes. No SQL, no network
# listener, no client/server protocol.
add_executable(db_engine
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/catalog/catalog.cpp
  src/db/query/query_executor.cpp
  src/db/wal/wal.cpp
  src/db/database/database.cpp
  src/db/engine/db_engine.cpp
)

# R3: Manager/Router coordination layer + CLI (Digvijay). The Manager routes
# container lifecycle to the container runtime and database operations through
# Database/QueryExecutor; the Router validates argv and dispatches; `nexusdb_cli`
# is the thin front end. The CLI links both stacks.
add_executable(nexusdb_cli
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/catalog/catalog.cpp
  src/db/query/query_executor.cpp
  src/db/wal/wal.cpp
  src/db/database/database.cpp
  src/manager/manager.cpp
  src/manager/router.cpp
  src/cli/nexusdb_cli.cpp
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
)

# R3: Manager/Router unit + integration test (isolated temp data dir).
add_executable(manager_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/catalog/catalog.cpp
  src/db/query/query_executor.cpp
  src/db/wal/wal.cpp
  src/db/database/database.cpp
  src/manager/manager.cpp
  src/manager/router.cpp
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  tests/manager/manager_test.cpp
)

# R3: CLI end-to-end test. Drives the real CLI executable; needs the CLI binary
# and the db_engine workload available at run time.
add_executable(nexusdb_cli_test
  tests/cli/cli_test.cpp
)
add_dependencies(nexusdb_cli_test nexusdb_cli db_engine)

# P3: contract test for the db_engine executable, driven via the direct-exec
# API (POSIX). On Windows process_spawn_exec is ENOSYS, so it reports
# ENV-LIMITED.
add_executable(nexus_db_engine_test
  src/container/process.c
  tests/container/db_engine_test.c
)

# Both the engine-contract test and the container lifecycle test need the
# engine binary at run time (path passed via $NEXUSDB_DB_ENGINE).
add_dependencies(nexus_db_engine_test db_engine)
add_dependencies(nexus_container_test db_engine)

# P4: controlled CPU workload helper (POSIX; no-op on Windows).
add_executable(nexus_cpu_workload tests/container/cpu_workload.c)

# P4: cgroup v2 CPU enforcement test. Runs the CPU workload unrestricted and
# throttled (cpu.max 20%) in disposable cgroups and compares kernel cpu.stat
# (nr_throttled / throttled_usec). Requires a real writable cgroup v2 hierarchy;
# otherwise it reports ENV-LIMITED rather than passing.
add_executable(nexus_cpu_enforce_test
  src/container/cgroup.c
  src/container/process.c
  tests/container/cpu_enforce_test.c
)
add_dependencies(nexus_cpu_enforce_test nexus_cpu_workload)

# P4: controlled memory-pressure workload helper (POSIX; no-op on Windows).
add_executable(nexus_mem_workload tests/container/mem_workload.c)

# P4: cgroup v2 memory enforcement test. Drives the memory workload past
# memory.max (swap disabled) to force a kernel OOM kill and reads
# memory.events; also covers "within limit" and plain allocation failure.
add_executable(nexus_mem_enforce_test
  src/container/cgroup.c
  src/container/process.c
  tests/container/mem_enforce_test.c
)
add_dependencies(nexus_mem_enforce_test nexus_mem_workload)

# Phase 4: benchmark harness (Rehan). The C orchestrator builds everywhere along
# with a portable CLI stub; the engine-backed 'nexus_benchmark' links the real
# DB engine (Linux, GCC 11+) and defines NEXUS_HAVE_DB_ENGINE. namespace.c is
# linked because container.c (P2) drives the gated namespaced child.
add_executable(nexus_benchmark_harness_test
  src/benchmark/harness.c
  src/bridge/adaptation_policy.c
  src/bridge/resource_monitor.c
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  tests/benchmark/harness_test.c
)
add_dependencies(nexus_benchmark_harness_test nexus_cpu_workload nexus_mem_workload)

# Portable benchmark CLI stub: builds everywhere but refuses to emit numbers
# without the DB engine.
add_executable(nexus_benchmark_stub
  src/benchmark/harness.c
  src/bridge/adaptation_policy.c
  src/bridge/resource_monitor.c
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  benchmarks/nexus_benchmark.c
)

# Engine-backed benchmark: exercises the real database executable through the
# production adaptation adapter.
add_executable(nexus_benchmark
  src/benchmark/harness.c
  src/bridge/adaptation_policy.c
  src/bridge/resource_monitor.c
  src/container/container.c
  src/container/cgroup.c
  src/container/namespace.c
  src/container/process.c
  src/benchmark/engine_workload.cpp
  src/bridge/buffer_pool_adapter.cpp
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  benchmarks/nexus_benchmark.c
)
target_compile_definitions(nexus_benchmark PRIVATE NEXUS_HAVE_DB_ENGINE)
add_dependencies(nexus_benchmark db_engine)

# R4: OS-DB Bridge <-> Database engine integration test (Digvijay). Links the
# real adaptation policy + resize adapter to the real engine: bridge-requested
# resize applied to a live BufferPool (and to a live Database), pinned-frame
# shrink refusal, dirty-eviction write-ahead ordering, record correctness across
# resize + WAL recovery, and dashboard engine metrics from a real pool.
add_executable(engine_adaptation_test
  src/bridge/adaptation_policy.c
  src/bridge/buffer_pool_adapter.cpp
  src/dashboard/metrics.c
  src/dashboard/dashboard_text.c
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/record/slotted_page.cpp
  src/db/record/heap_table.cpp
  src/db/catalog/catalog.cpp
  src/db/query/query_executor.cpp
  src/db/wal/wal.cpp
  src/db/database/database.cpp
  tests/bridge/engine_adaptation_test.cpp
)

enable_testing()
add_test(NAME container_lifecycle COMMAND nexus_container_test)
add_test(NAME container_engine_demo COMMAND container_engine_demo)
add_test(NAME cgroup_reliability COMMAND nexus_cgroup_test)
add_test(NAME process_exec COMMAND nexus_process_test)
add_test(NAME namespace_isolation COMMAND nexus_namespace_test)
add_test(NAME db_engine_binary COMMAND nexus_db_engine_test)
add_test(NAME bridge_resource_monitor COMMAND nexus_bridge_test)
add_test(NAME bridge_adaptation_policy COMMAND nexus_policy_test)
add_test(NAME dashboard_metrics COMMAND nexus_dashboard_test)
add_test(NAME dashboard_demo COMMAND nexus_dashboard_demo)
add_test(NAME benchmark_harness COMMAND nexus_benchmark_harness_test)
add_test(NAME db_storage COMMAND db_storage_test)
add_test(NAME slotted_page COMMAND slotted_page_test)
add_test(NAME heap_table COMMAND heap_table_test)
add_test(NAME catalog COMMAND catalog_test)
add_test(NAME query_executor COMMAND query_executor_test)
add_test(NAME wal COMMAND wal_test)
add_test(NAME recovery COMMAND recovery_test)
add_test(NAME buffer_pool COMMAND buffer_pool_test)
add_test(NAME buffer_pool_demo COMMAND buffer_pool_demo)
add_test(NAME nexusdb_engine_demo COMMAND nexusdb_engine_demo)
add_test(NAME cpu_enforcement COMMAND nexus_cpu_enforce_test)
add_test(NAME memory_enforcement COMMAND nexus_mem_enforce_test)
add_test(NAME manager COMMAND manager_test)
add_test(NAME cli COMMAND nexusdb_cli_test)
add_test(NAME engine_adaptation COMMAND engine_adaptation_test)

# Point the container and engine-contract tests at the freshly built engine so
# their configured-engine checks run for real (instead of ENV-LIMITED).
set_tests_properties(container_lifecycle db_engine_binary PROPERTIES
  ENVIRONMENT "NEXUSDB_DB_ENGINE=$<TARGET_FILE:db_engine>")

# Point the CLI end-to-end test at the freshly built CLI + engine binaries.
set_tests_properties(cli PROPERTIES
  ENVIRONMENT "NEXUSDB_CLI=$<TARGET_FILE:nexusdb_cli>;NEXUSDB_DB_ENGINE=$<TARGET_FILE:db_engine>")

# Point the P4 enforcement tests at their freshly built workload helpers.
set_tests_properties(cpu_enforcement PROPERTIES
  ENVIRONMENT "NEXUS_CPU_WORKLOAD=$<TARGET_FILE:nexus_cpu_workload>")
set_tests_properties(memory_enforcement PROPERTIES
  ENVIRONMENT "NEXUS_MEM_WORKLOAD=$<TARGET_FILE:nexus_mem_workload>")
