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
                       $(BUILD_DIR)/process.o \
                       $(BUILD_DIR)/container_demo.o

CGROUP_OBJS := $(BUILD_DIR)/cgroup.o \
               $(BUILD_DIR)/process.o \
               $(BUILD_DIR)/cgroup_test.o

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

# Phase 1 OS-DB Bridge (Rehan): cgroup v2 resource monitoring, C11.
# Reuses the container cgroup controller + runtime, so those objects are linked.
BRIDGE_TEST_BIN := $(BUILD_DIR)/nexus_bridge_test$(EXE)
BRIDGE_OBJS := $(BUILD_DIR)/resource_monitor.o \
               $(BUILD_DIR)/container.o \
               $(BUILD_DIR)/cgroup.o \
               $(BUILD_DIR)/process.o \
               $(BUILD_DIR)/bridge_test.o

# Phase 2 OS-DB Bridge (Rehan): memory-pressure adaptation policy, C11.
# Pure policy; the test builds snapshots directly, so no container objects.
POLICY_TEST_BIN := $(BUILD_DIR)/nexus_policy_test$(EXE)
POLICY_OBJS := $(BUILD_DIR)/adaptation_policy.o \
               $(BUILD_DIR)/policy_test.o

all: $(TEST_BIN) $(CGROUP_TEST_BIN) $(DB_TEST_BIN) $(BUFFER_TEST_BIN) $(CONTAINER_DEMO_BIN) $(BRIDGE_TEST_BIN) $(POLICY_TEST_BIN)

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

test: $(TEST_BIN) $(CGROUP_TEST_BIN) $(DB_TEST_BIN) $(BUFFER_TEST_BIN) $(CONTAINER_DEMO_BIN) $(BRIDGE_TEST_BIN) $(POLICY_TEST_BIN)
	$(TEST_BIN)
	$(CGROUP_TEST_BIN)
	$(DB_TEST_BIN)
	$(BUFFER_TEST_BIN)
	$(CONTAINER_DEMO_BIN)
	$(BRIDGE_TEST_BIN)
	$(POLICY_TEST_BIN)

clean:
ifeq ($(OS),Windows_NT)
	-if exist $(BUILD_DIR) rmdir /S /Q $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR)
endif

.PHONY: all test clean adapter
