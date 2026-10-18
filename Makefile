# NexusDB — Container Engine (foundation) build.
#
# Targets:
#   make            build all test binaries
#   make test       build + run all tests (container + db storage)
#   make clean      remove build artefacts
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

# Part 2: real process-launch test (configured exe, self-attach, namespaces).
PROCESS_TEST_BIN := $(BUILD_DIR)/nexus_process_test$(EXE)

# Bridge milestone: isolated memory-telemetry relay test (no sudo, NEXUS_CGROUP_ROOT).
MEMORY_BRIDGE_TEST_BIN := $(BUILD_DIR)/nexus_memory_bridge_test$(EXE)

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

CGROUP_OBJS := $(BUILD_DIR)/cgroup.o \
               $(BUILD_DIR)/process.o \
               $(BUILD_DIR)/cgroup_test.o

PROCESS_OBJS := $(BUILD_DIR)/container.o \
                $(BUILD_DIR)/cgroup.o \
                $(BUILD_DIR)/namespace.o \
                $(BUILD_DIR)/process.o \
                $(BUILD_DIR)/process_test.o

MEMORY_BRIDGE_OBJS := $(BUILD_DIR)/container.o \
                      $(BUILD_DIR)/cgroup.o \
                      $(BUILD_DIR)/namespace.o \
                      $(BUILD_DIR)/process.o \
                      $(BUILD_DIR)/memory_bridge.o \
                      $(BUILD_DIR)/test_memory_bridge.o

# Phase 1 DB storage (Digvijay): Page + PageStorage, C++17.
DB_TEST_BIN := $(BUILD_DIR)/db_storage_test$(EXE)
DB_OBJS := $(BUILD_DIR)/page.o \
           $(BUILD_DIR)/page_storage.o \
           $(BUILD_DIR)/storage_test.o

all: $(TEST_BIN) $(CGROUP_TEST_BIN) $(PROCESS_TEST_BIN) $(MEMORY_BRIDGE_TEST_BIN) $(DB_TEST_BIN)

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

$(BUILD_DIR)/cgroup_test.o: tests/container/cgroup_test.c include/container/cgroup.h include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(CGROUP_TEST_BIN): $(CGROUP_OBJS)
	$(CC) $(CFLAGS) $(CGROUP_OBJS) -o $@

$(BUILD_DIR)/process_test.o: tests/container/process_test.c include/container/container.h include/container/cgroup.h include/container/namespace.h include/container/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(PROCESS_TEST_BIN): $(PROCESS_OBJS)
	$(CC) $(CFLAGS) $(PROCESS_OBJS) -o $@

$(BUILD_DIR)/memory_bridge.o: src/bridge/memory_bridge.c include/bridge/memory_bridge.h include/container/container.h include/container/cgroup.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_memory_bridge.o: tests/bridge/test_memory_bridge.c include/bridge/memory_bridge.h include/container/container.h include/container/cgroup.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(MEMORY_BRIDGE_TEST_BIN): $(MEMORY_BRIDGE_OBJS)
	$(CC) $(CFLAGS) $(MEMORY_BRIDGE_OBJS) -o $@

$(BUILD_DIR)/page.o: src/db/storage/page.cpp include/db/page.h include/common/config.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/page_storage.o: src/db/storage/page_storage.cpp include/db/page_storage.h include/db/page.h include/common/error.h include/common/config.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/storage_test.o: tests/db/storage_test.cpp include/db/page_storage.h include/db/page.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(DB_TEST_BIN): $(DB_OBJS)
	$(CXX) $(CXXFLAGS) $(DB_OBJS) -o $@

test: $(TEST_BIN) $(CGROUP_TEST_BIN) $(PROCESS_TEST_BIN) $(MEMORY_BRIDGE_TEST_BIN) $(DB_TEST_BIN)
	$(TEST_BIN)
	$(CGROUP_TEST_BIN)
	$(PROCESS_TEST_BIN)
	$(MEMORY_BRIDGE_TEST_BIN)
	$(DB_TEST_BIN)

clean:
ifeq ($(OS),Windows_NT)
	-if exist $(BUILD_DIR) rmdir /S /Q $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR)
endif

.PHONY: all test clean
