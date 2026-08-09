cmake_minimum_required(VERSION 3.15)

project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_HTTP "Build HTTP API server (pp-http)" OFF)

# Test build option (default OFF) - uses vendored googletest under third_party/
option(BUILD_TESTING "Build the testing tree" OFF)

if(BUILD_TESTING)
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
  enable_testing()
endif()

# Root of the source tree (headers use paths like "lib/common/Logger.h")
set(PP_SRC_DIR "${CMAKE_SOURCE_DIR}/src")

# Apply -Werror only to project sources (not third_party / vendored deps).
add_subdirectory(src)
