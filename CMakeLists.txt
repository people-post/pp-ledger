cmake_minimum_required(VERSION 3.15)

project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_HTTP "Build HTTP API server (pp-http)" OFF)

# Treat warnings as errors (vendored targets opt out as needed)
add_compile_options(-Werror)

# Test build option (default OFF) - gtest only fetched when enabled
option(BUILD_TESTING "Build the testing tree" OFF)

if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
endif()

# Root of the source tree (headers use paths like "lib/common/Logger.h")
set(PP_SRC_DIR "${CMAKE_SOURCE_DIR}/src")

add_subdirectory(src)
