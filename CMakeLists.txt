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
  src/container/process.c
  examples/container/container_engine_demo.c
)

# Part 1: isolated cgroup reliability test (NEXUS_CGROUP_ROOT, no sudo).
add_executable(nexus_cgroup_test
  src/container/cgroup.c
  src/container/process.c
  tests/container/cgroup_test.c
)

# Phase 1: OS-DB Bridge resource monitoring (Rehan). Reuses the container
# cgroup controller + runtime to read real cgroup v2 statistics and exposes
# explicit per-field availability (no fabricated values).
add_executable(nexus_bridge_test
  src/bridge/resource_monitor.c
  src/container/container.c
  src/container/cgroup.c
  src/container/process.c
  tests/bridge/resource_monitor_test.c
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

enable_testing()
add_test(NAME container_lifecycle COMMAND nexus_container_test)
add_test(NAME container_engine_demo COMMAND container_engine_demo)
add_test(NAME cgroup_reliability COMMAND nexus_cgroup_test)
add_test(NAME bridge_resource_monitor COMMAND nexus_bridge_test)
add_test(NAME db_storage COMMAND db_storage_test)
add_test(NAME buffer_pool COMMAND buffer_pool_test)
add_test(NAME buffer_pool_demo COMMAND buffer_pool_demo)
add_test(NAME nexusdb_engine_demo COMMAND nexusdb_engine_demo)
