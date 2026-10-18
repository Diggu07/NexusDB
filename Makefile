# NexusDB — Container Engine (foundation) build.
#
# Targets:
#   make            build all test binaries + the container demo
#   make test       build + run all tests (container + cgroup + db storage + buffer pool + demo)
#   make clean      remove build artefacts
#
# Note: the DB engine demos (buffer_pool_demo, nexusdb_engine_demo) are built
# and run by the CMake path only; this Makefile runs the container demo.
#
# Linux (Ubuntu/WSL2, the real target):
#   make test
# Windows dev machine (no WSL here — lifecycle shim only):
#   mingw32-make test
#
# Later phases (namespaces, cgroups, real DB launcher) extend this file;
# the C++ DB engine (Digvijay) and Bridge/dashboard (Rehan) keep their own
# build files — nothing here touches their code.

# 'make' defaults CC to 'cc', which often does not exist on Windows/MinGW
# while 'gcc' does; command-line 'make CC=...' still overrides this.
ifeq ($(origin CC),default)
CC = gcc
endif
ifeq ($(origin CXX),default)
CXX = g++
endif
CFLAGS  ?= -Wall -Wextra -Werror -std=c11
CXXFLAGS ?= -Wall -Wextra -Werror -std=c++17
CPPFLAGS += -Iinclude

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

BUILD_DIR := build
TEST_BIN  := $(BUILD_DIR)/nexus_container_test$(EXE)

# Part 1: isolated cgroup reliability test (no sudo, NEXUS_CGROUP_ROOT).
CGROUP_TEST_BIN := $(BUILD_DIR)/nexus_cgroup_test$(EXE)

# P0: direct process-execution API test (POSIX; Windows reports ENOSYS).
PROCESS_TEST_BIN := $(BUILD_DIR)/nexus_process_test$(EXE)

SRCS := src/container/container.c \
        src/container/cgroup.c \
        src/container/namespace.c \
        src/container/process.c \
        tests/container/container_test.c
OBJS := $(BUILD_DIR)/container.o \
        $(BUILD_DIR)/cgroup.o \
        $(BUILD_DIR)/namespace.o \
        $(BUILD_DIR)/process.o \
        $(BUILD_DIR)/container_test.o

# Container Engine demonstration (Ayush's runtime + an examples/ driver):
# links the real container + placeholder-process sources, plus cgroup.c since
# the integrated lifecycle provisions cgroups in create/start/delete.
CONTAINER_DEMO_BIN := $(BUILD_DIR)/container_engine_demo$(EXE)
CONTAINER_DEMO_OBJS := $(BUILD_DIR)/container.o \
                       $(BUILD_DIR)/cgroup.o \
                       $(BUILD_DIR)/namespace.o \
                       $(BUILD_DIR)/process.o \
                       $(BUILD_DIR)/container_demo.o

CGROUP_OBJS := $(BUILD_DIR)/cgroup.o \
               $(BUILD_DIR)/process.o \
               $(BUILD_DIR)/cgroup_test.o

PROCESS_OBJS := $(BUILD_DIR)/process.o \
                $(BUILD_DIR)/process_test.o

# P2: Linux namespace integration test (env-limited when unprivileged).
NS_TEST_BIN := $(BUILD_DIR)/nexus_namespace_test$(EXE)
NS_OBJS := $(BUILD_DIR)/process.o \
           $(BUILD_DIR)/namespace.o \
           $(BUILD_DIR)/namespace_test.o

# Phase 1 DB storage (Digvijay): Page + PageStorage, C++17.
DB_TEST_BIN := $(BUILD_DIR)/db_storage_test$(EXE)
DB_OBJS := $(BUILD_DIR)/page.o \
           $(BUILD_DIR)/page_storage.o \
           $(BUILD_DIR)/storage_test.o

# Phase 2 DB buffer pool (Digvijay): BufferPool over PageStorage, C++17.
BUFFER_TEST_BIN := $(BUILD_DIR)/buffer_pool_test$(EXE)
BUFFER_OBJS := $(BUILD_DIR)/page.o \
               $(BUILD_DIR)/page_storage.o \
               $(BUILD_DIR)/buffer_pool.o \
               $(BUILD_DIR)/buffer_pool_test.o

# P3: the real DB-engine executable (Digvijay) launched by the container as its
# workload. Minimal: opens PageStorage + BufferPool and idles until SIGTERM.
ENGINE_BIN := $(BUILD_DIR)/db_engine$(EXE)
ENGINE_OBJS := $(BUILD_DIR)/page.o \
               $(BUILD_DIR)/page_storage.o \
               $(BUILD_DIR)/buffer_pool.o \
               $(BUILD_DIR)/db_engine.o

# P3: db_engine executable contract test (direct-exec, POSIX).
ENG_TEST_BIN := $(BUILD_DIR)/nexus_db_engine_test$(EXE)
ENG_TEST_OBJS := $(BUILD_DIR)/process.o \
                 $(BUILD_DIR)/db_engine_test.o

# The container lifecycle test and the engine-contract test locate the built
# engine via $NEXUSDB_DB_ENGINE (path relative to the make cwd).
export NEXUSDB_DB_ENGINE := $(ENGINE_BIN)

# P4: controlled workload helpers + cgroup v2 CPU/memory enforcement tests.
# The enforcement tests need a real writable cgroup v2 hierarchy; without one
# they report ENV-LIMITED (exit 0) rather than passing.
CPU_WORKLOAD_BIN := $(BUILD_DIR)/nexus_cpu_workload$(EXE)
MEM_WORKLOAD_BIN := $(BUILD_DIR)/nexus_mem_workload$(EXE)
CPU_ENFORCE_BIN := $(BUILD_DIR)/nexus_cpu_enforce_test$(EXE)
MEM_ENFORCE_BIN := $(BUILD_DIR)/nexus_mem_enforce_test$(EXE)
CPU_ENFORCE_OBJS := $(BUILD_DIR)/cgroup.o \
                    $(BUILD_DIR)/process.o \
                    $(BUILD_DIR)/cpu_enforce_test.o
MEM_ENFORCE_OBJS := $(BUILD_DIR)/cgroup.o \
                    $(BUILD_DIR)/process.o \
                    $(BUILD_DIR)/mem_enforce_test.o

# The enforcement tests locate their workload helpers via these paths.
export NEXUS_CPU_WORKLOAD := $(CPU_WORKLOAD_BIN)
export NEXUS_MEM_WORKLOAD := $(MEM_WORKLOAD_BIN)

# Phase 1 OS-DB Bridge (Rehan): cgroup v2 resource monitoring, C11.
# Reuses the container cgroup controller + runtime, so those objects are linked
# (namespace.o included because container.c drives the gated namespaced child).
BRIDGE_TEST_BIN := $(BUILD_DIR)/nexus_bridge_test$(EXE)
BRIDGE_OBJS := $(BUILD_DIR)/resource_monitor.o \
               $(BUILD_DIR)/container.o \
               $(BUILD_DIR)/cgroup.o \
               $(BUILD_DIR)/namespace.o \
               $(BUILD_DIR)/process.o \
               $(BUILD_DIR)/bridge_test.o

# Phase 2 OS-DB Bridge (Rehan): memory-pressure adaptation policy, C11.
# Pure policy; the test builds snapshots directly, so no container objects.
POLICY_TEST_BIN := $(BUILD_DIR)/nexus_policy_test$(EXE)
POLICY_OBJS := $(BUILD_DIR)/adaptation_policy.o \
               $(BUILD_DIR)/policy_test.o

# Phase 3 Live Dashboard (Rehan): metrics + plain-text renderer, C11.
# The plain build needs no ncurses; the ncurses UI is a separate opt-in target.
DASH_TEST_BIN := $(BUILD_DIR)/nexus_dashboard_test$(EXE)
DASH_OBJS := $(BUILD_DIR)/dash_metrics.o \
             $(BUILD_DIR)/dashboard_text.o \
             $(BUILD_DIR)/adaptation_policy.o \
             $(BUILD_DIR)/dashboard_test.o
DASH_DEMO_BIN := $(BUILD_DIR)/nexus_dashboard_demo$(EXE)
DASH_DEMO_OBJS := $(BUILD_DIR)/dash_metrics.o \
                  $(BUILD_DIR)/dashboard_text.o \
                  $(BUILD_DIR)/dashboard_demo.o \
                  $(BUILD_DIR)/resource_monitor.o \
                  $(BUILD_DIR)/adaptation_policy.o \
                  $(BUILD_DIR)/container.o \
                  $(BUILD_DIR)/cgroup.o \
                  $(BUILD_DIR)/namespace.o \
                  $(BUILD_DIR)/process.o

# Phase 4 Benchmark harness (Rehan): C11 orchestration + a portable CLI stub.
# The engine-backed benchmark ('make benchmark', Linux/GCC 11+) links the DB
# engine; the stub compiles everywhere and refuses to run without it.
HARNESS_TEST_BIN := $(BUILD_DIR)/nexus_benchmark_harness_test$(EXE)
HARNESS_TEST_OBJS := $(BUILD_DIR)/harness.o \
                     $(BUILD_DIR)/adaptation_policy.o \
                     $(BUILD_DIR)/resource_monitor.o \
                     $(BUILD_DIR)/container.o \
                     $(BUILD_DIR)/cgroup.o \
                     $(BUILD_DIR)/namespace.o \
                     $(BUILD_DIR)/process.o \
                     $(BUILD_DIR)/harness_test.o
BENCH_STUB_BIN := $(BUILD_DIR)/nexus_benchmark_stub$(EXE)
BENCH_STUB_OBJS := $(BUILD_DIR)/harness.o \
                   $(BUILD_DIR)/adaptation_policy.o \
                   $(BUILD_DIR)/resource_monitor.o \
                   $(BUILD_DIR)/container.o \
                   $(BUILD_DIR)/cgroup.o \
                   $(BUILD_DIR)/namespace.o \
                   $(BUILD_DIR)/process.o \
                   $(BUILD_DIR)/benchmark_main.o
BENCH_ENGINE_OBJS := $(BUILD_DIR)/harness.o \
                     $(BUILD_DIR)/adaptation_policy.o \
                     $(BUILD_DIR)/resource_monitor.o \
                     $(BUILD_DIR)/container.o \
                     $(BUILD_DIR)/cgroup.o \
                     $(BUILD_DIR)/namespace.o \
                     $(BUILD_DIR)/process.o \
                     $(BUILD_DIR)/engine_workload.o \
                     $(BUILD_DIR)/buffer_pool_adapter.o \
                     $(BUILD_DIR)/page.o \
                     $(BUILD_DIR)/page_storage.o \
                     $(BUILD_DIR)/buffer_pool.o \
                     $(BUILD_DIR)/benchmark_main_engine.o

all: $(TEST_BIN) $(CGROUP_TEST_BIN) $(PROCESS_TEST_BIN) $(NS_TEST_BIN) $(DB_TEST_BIN) $(BUFFER_TEST_BIN) $(CONTAINER_DEMO_BIN) $(ENGINE_BIN) $(ENG_TEST_BIN) $(CPU_WORKLOAD_BIN) $(MEM_WORKLOAD_BIN) $(CPU_ENFORCE_BIN) $(MEM_ENFORCE_BIN) $(BRIDGE_TEST_BIN) $(POLICY_TEST_BIN) $(DASH_TEST_BIN) $(DASH_DEMO_BIN) $(HARNESS_TEST_BIN) $(BENCH_STUB_BIN)

$(BUILD_DIR):
ifeq ($(OS),Windows_NT)
	if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)
else
	mkdir -p $(BUILD_DIR)
endif

$(BUILD_DIR)/container.o: src/container/container.c include/container/container.h include/container/cgroup.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/cgroup.o: src/container/cgroup.c include/container/cgroup.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/namespace.o: src/container/namespace.c include/container/namespace.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/process.o: src/container/process.c include/container/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/container_test.o: tests/container/container_test.c include/container/container.h include/container/cgroup.h include/container/namespace.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/container_demo.o: examples/container/container_engine_demo.c include/container/container.h include/container/cgroup.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(CONTAINER_DEMO_BIN): $(CONTAINER_DEMO_OBJS)
	$(CC) $(CFLAGS) $(CONTAINER_DEMO_OBJS) -o $@

$(BUILD_DIR)/cgroup_test.o: tests/container/cgroup_test.c include/container/cgroup.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(CGROUP_TEST_BIN): $(CGROUP_OBJS)
	$(CC) $(CFLAGS) $(CGROUP_OBJS) -o $@

$(BUILD_DIR)/process_test.o: tests/container/process_test.c include/container/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(PROCESS_TEST_BIN): $(PROCESS_OBJS)
	$(CC) $(CFLAGS) $(PROCESS_OBJS) -o $@

$(BUILD_DIR)/namespace_test.o: tests/container/namespace_test.c include/container/namespace.h include/container/process.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(NS_TEST_BIN): $(NS_OBJS)
	$(CC) $(CFLAGS) $(NS_OBJS) -o $@

$(BUILD_DIR)/resource_monitor.o: src/bridge/resource_monitor.c include/bridge/resource_monitor.h include/container/container.h include/container/cgroup.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/bridge_test.o: tests/bridge/resource_monitor_test.c include/bridge/resource_monitor.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BRIDGE_TEST_BIN): $(BRIDGE_OBJS)
	$(CC) $(CFLAGS) $(BRIDGE_OBJS) -o $@

$(BUILD_DIR)/adaptation_policy.o: src/bridge/adaptation_policy.c include/bridge/adaptation_policy.h include/bridge/resource_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/policy_test.o: tests/bridge/adaptation_policy_test.c include/bridge/adaptation_policy.h include/bridge/resource_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(POLICY_TEST_BIN): $(POLICY_OBJS)
	$(CC) $(CFLAGS) $(POLICY_OBJS) -o $@

$(BUILD_DIR)/dash_metrics.o: src/dashboard/metrics.c include/dashboard/metrics.h include/bridge/adaptation_policy.h include/bridge/resource_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dashboard_text.o: src/dashboard/dashboard_text.c include/dashboard/dashboard.h include/dashboard/metrics.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dashboard_test.o: tests/dashboard/dashboard_test.c include/dashboard/dashboard.h include/dashboard/metrics.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(DASH_TEST_BIN): $(DASH_OBJS)
	$(CC) $(CFLAGS) $(DASH_OBJS) -o $@

$(BUILD_DIR)/dashboard_demo.o: examples/dashboard/dashboard_demo.c include/dashboard/dashboard.h include/dashboard/metrics.h include/bridge/adaptation_policy.h include/bridge/resource_monitor.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(DASH_DEMO_BIN): $(DASH_DEMO_OBJS)
	$(CC) $(CFLAGS) $(DASH_DEMO_OBJS) -o $@

# Optional live ncurses UI (Linux/WSL2 + libncurses-dev). Kept out of
# all/test: the default build works without ncurses. Requires the demo and the
# ncurses renderer compiled with -DNEXUS_HAVE_NCURSES.
$(BUILD_DIR)/dashboard_demo_nc.o: examples/dashboard/dashboard_demo.c include/dashboard/dashboard.h include/dashboard/metrics.h include/bridge/adaptation_policy.h include/bridge/resource_monitor.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DNEXUS_HAVE_NCURSES -c $< -o $@

$(BUILD_DIR)/dashboard_ncurses.o: src/dashboard/dashboard_ncurses.c include/dashboard/dashboard.h include/dashboard/metrics.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DNEXUS_HAVE_NCURSES -c $< -o $@

dashboard-ncurses: $(BUILD_DIR)/dashboard_ncurses.o $(BUILD_DIR)/dashboard_demo_nc.o $(BUILD_DIR)/dash_metrics.o $(BUILD_DIR)/dashboard_text.o $(BUILD_DIR)/resource_monitor.o $(BUILD_DIR)/adaptation_policy.o $(BUILD_DIR)/container.o $(BUILD_DIR)/cgroup.o $(BUILD_DIR)/namespace.o $(BUILD_DIR)/process.o
	$(CC) $(CFLAGS) $(BUILD_DIR)/dashboard_ncurses.o $(BUILD_DIR)/dashboard_demo_nc.o $(BUILD_DIR)/dash_metrics.o $(BUILD_DIR)/dashboard_text.o $(BUILD_DIR)/resource_monitor.o $(BUILD_DIR)/adaptation_policy.o $(BUILD_DIR)/container.o $(BUILD_DIR)/cgroup.o $(BUILD_DIR)/namespace.o $(BUILD_DIR)/process.o -o $(BUILD_DIR)/nexus_dashboard$(EXE) -lncurses

# Phase 4 Benchmark harness (Rehan). 'benchmark' (engine-backed) is Linux-only;
# harness_test and the stub build everywhere and are part of all/test.
$(BUILD_DIR)/harness.o: src/benchmark/harness.c include/benchmark/harness.h include/bridge/adaptation_policy.h include/bridge/resource_monitor.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/harness_test.o: tests/benchmark/harness_test.c include/benchmark/harness.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(HARNESS_TEST_BIN): $(HARNESS_TEST_OBJS)
	$(CC) $(CFLAGS) $(HARNESS_TEST_OBJS) -o $@

$(BUILD_DIR)/benchmark_main.o: benchmarks/nexus_benchmark.c include/benchmark/harness.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BENCH_STUB_BIN): $(BENCH_STUB_OBJS)
	$(CC) $(CFLAGS) $(BENCH_STUB_OBJS) -o $@

$(BUILD_DIR)/benchmark_main_engine.o: benchmarks/nexus_benchmark.c include/benchmark/harness.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DNEXUS_HAVE_DB_ENGINE -c $< -o $@

$(BUILD_DIR)/engine_workload.o: src/benchmark/engine_workload.cpp include/benchmark/harness.h include/bridge/buffer_pool_adapter.h include/db/buffer_pool.h include/db/page.h include/db/page_storage.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

benchmark: $(BENCH_ENGINE_OBJS)
	$(CXX) $(CXXFLAGS) $(BENCH_ENGINE_OBJS) -o $(BUILD_DIR)/nexus_benchmark$(EXE)

# Phase 2 C++ integration adapter. Compile-checked here; it links on the DB
# engine side, which needs GCC 11+ (unavailable on this dev machine). Kept out
# of 'all'/'test' because it has no standalone executable.
$(BUILD_DIR)/buffer_pool_adapter.o: src/bridge/buffer_pool_adapter.cpp include/bridge/buffer_pool_adapter.h include/bridge/adaptation_policy.h include/db/buffer_pool.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

adapter: $(BUILD_DIR)/buffer_pool_adapter.o

$(BUILD_DIR)/page.o: src/db/storage/page.cpp include/db/page.h include/common/config.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/page_storage.o: src/db/storage/page_storage.cpp include/db/page_storage.h include/db/page.h include/common/error.h include/common/config.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/storage_test.o: tests/db/storage_test.cpp include/db/page_storage.h include/db/page.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(DB_TEST_BIN): $(DB_OBJS)
	$(CXX) $(CXXFLAGS) $(DB_OBJS) -o $@

$(BUILD_DIR)/buffer_pool.o: src/db/buffer/buffer_pool.cpp include/db/buffer_pool.h include/db/page.h include/db/page_storage.h include/common/error.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/buffer_pool_test.o: tests/db/buffer_pool_test.cpp include/db/buffer_pool.h include/db/page.h include/db/page_storage.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUFFER_TEST_BIN): $(BUFFER_OBJS)
	$(CXX) $(CXXFLAGS) $(BUFFER_OBJS) -o $@

$(BUILD_DIR)/db_engine.o: src/db/engine/db_engine.cpp include/db/buffer_pool.h include/db/page.h include/db/page_storage.h include/common/error.h include/common/config.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(ENGINE_BIN): $(ENGINE_OBJS)
	$(CXX) $(CXXFLAGS) $(ENGINE_OBJS) -o $@

$(BUILD_DIR)/db_engine_test.o: tests/container/db_engine_test.c include/container/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(ENG_TEST_BIN): $(ENG_TEST_OBJS)
	$(CC) $(CFLAGS) $(ENG_TEST_OBJS) -o $@

$(BUILD_DIR)/cpu_workload.o: tests/container/cpu_workload.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(CPU_WORKLOAD_BIN): $(BUILD_DIR)/cpu_workload.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/mem_workload.o: tests/container/mem_workload.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(MEM_WORKLOAD_BIN): $(BUILD_DIR)/mem_workload.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/cpu_enforce_test.o: tests/container/cpu_enforce_test.c include/container/cgroup.h include/container/container.h include/container/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(CPU_ENFORCE_BIN): $(CPU_ENFORCE_OBJS) $(CPU_WORKLOAD_BIN)
	$(CC) $(CFLAGS) $(CPU_ENFORCE_OBJS) -o $@

$(BUILD_DIR)/mem_enforce_test.o: tests/container/mem_enforce_test.c include/container/cgroup.h include/container/container.h include/container/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(MEM_ENFORCE_BIN): $(MEM_ENFORCE_OBJS) $(MEM_WORKLOAD_BIN)
	$(CC) $(CFLAGS) $(MEM_ENFORCE_OBJS) -o $@

test: $(TEST_BIN) $(CGROUP_TEST_BIN) $(PROCESS_TEST_BIN) $(NS_TEST_BIN) $(DB_TEST_BIN) $(BUFFER_TEST_BIN) $(CONTAINER_DEMO_BIN) $(ENGINE_BIN) $(ENG_TEST_BIN) $(CPU_WORKLOAD_BIN) $(MEM_WORKLOAD_BIN) $(CPU_ENFORCE_BIN) $(MEM_ENFORCE_BIN) $(BRIDGE_TEST_BIN) $(POLICY_TEST_BIN) $(DASH_TEST_BIN) $(DASH_DEMO_BIN) $(HARNESS_TEST_BIN)
	$(TEST_BIN)
	$(CGROUP_TEST_BIN)
	$(PROCESS_TEST_BIN)
	$(NS_TEST_BIN)
	$(DB_TEST_BIN)
	$(BUFFER_TEST_BIN)
	$(CONTAINER_DEMO_BIN)
	$(ENG_TEST_BIN)
	$(CPU_ENFORCE_BIN)
	$(MEM_ENFORCE_BIN)
	$(BRIDGE_TEST_BIN)
	$(POLICY_TEST_BIN)
	$(DASH_TEST_BIN)
	$(DASH_DEMO_BIN)
	$(HARNESS_TEST_BIN)

clean:
ifeq ($(OS),Windows_NT)
	-if exist $(BUILD_DIR) rmdir /S /Q $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR)
endif

.PHONY: all test clean adapter dashboard-ncurses benchmark
