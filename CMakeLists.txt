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

project(NexusDB LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Werror)
endif()

include_directories(include)

add_executable(nexus_container_test
  src/container/container.c
  src/container/process.c
  tests/container/container_test.c
)

enable_testing()
add_test(NAME container_lifecycle COMMAND nexus_container_test)
