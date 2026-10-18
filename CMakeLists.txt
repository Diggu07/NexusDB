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

# Phase 1: Page + PageStorage (Digvijay, Database Engine).
add_executable(db_storage_test
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  tests/db/storage_test.cpp
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

# P3: the real DB-engine executable the container launches as its workload.
# Minimal and honest: it opens PageStorage + BufferPool over the database file,
# stays alive until SIGTERM/SIGINT, then flushes and closes. No SQL, no network
# listener, no client/server protocol.
add_executable(db_engine
  src/db/storage/page.cpp
  src/db/storage/page_storage.cpp
  src/db/buffer/buffer_pool.cpp
  src/db/engine/db_engine.cpp
)

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

enable_testing()
add_test(NAME container_lifecycle COMMAND nexus_container_test)
add_test(NAME container_engine_demo COMMAND container_engine_demo)
add_test(NAME cgroup_reliability COMMAND nexus_cgroup_test)
add_test(NAME process_exec COMMAND nexus_process_test)
add_test(NAME namespace_isolation COMMAND nexus_namespace_test)
add_test(NAME db_engine_binary COMMAND nexus_db_engine_test)
add_test(NAME db_storage COMMAND db_storage_test)
add_test(NAME buffer_pool COMMAND buffer_pool_test)
add_test(NAME buffer_pool_demo COMMAND buffer_pool_demo)
add_test(NAME nexusdb_engine_demo COMMAND nexusdb_engine_demo)
add_test(NAME cpu_enforcement COMMAND nexus_cpu_enforce_test)
add_test(NAME memory_enforcement COMMAND nexus_mem_enforce_test)

# Point the container and engine-contract tests at the freshly built engine so
# their configured-engine checks run for real (instead of ENV-LIMITED).
set_tests_properties(container_lifecycle db_engine_binary PROPERTIES
  ENVIRONMENT "NEXUSDB_DB_ENGINE=$<TARGET_FILE:db_engine>")

# Point the P4 enforcement tests at their freshly built workload helpers.
set_tests_properties(cpu_enforcement PROPERTIES
  ENVIRONMENT "NEXUS_CPU_WORKLOAD=$<TARGET_FILE:nexus_cpu_workload>")
set_tests_properties(memory_enforcement PROPERTIES
  ENVIRONMENT "NEXUS_MEM_WORKLOAD=$<TARGET_FILE:nexus_mem_workload>")
