# NexusDB — Container Engine (foundation) build.
#
# Targets:
#   make            build the lifecycle test binary
#   make test       build + run the lifecycle test
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
CFLAGS  ?= -Wall -Wextra -Werror -std=c11
CPPFLAGS += -Iinclude

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

BUILD_DIR := build
TEST_BIN  := $(BUILD_DIR)/nexus_container_test$(EXE)

SRCS := src/container/container.c \
        src/container/process.c \
        tests/container/container_test.c
OBJS := $(BUILD_DIR)/container.o \
        $(BUILD_DIR)/process.o \
        $(BUILD_DIR)/container_test.o

all: $(TEST_BIN)

$(BUILD_DIR):
ifeq ($(OS),Windows_NT)
	if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)
else
	mkdir -p $(BUILD_DIR)
endif

$(BUILD_DIR)/container.o: src/container/container.c include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/process.o: src/container/process.c include/container/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/container_test.o: tests/container/container_test.c include/container/container.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

clean:
ifeq ($(OS),Windows_NT)
	-if exist $(BUILD_DIR) rmdir /S /Q $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR)
endif

.PHONY: all test clean
