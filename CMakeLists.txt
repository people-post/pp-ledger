# Root CMake options and library layout for pp-ledger.

cmake_minimum_required(VERSION 3.24)

project(pp-ledger VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PP_LEDGER_BUILD_APPS "Build pp-beacon, pp-miner, pp-client, pp-relay" ON)
option(PP_LEDGER_BUILD_HTTP "Build HTTP API server (pp-http)" OFF)
option(PP_LEDGER_BUILD_TESTS "Build the testing tree" OFF)

# Back-compat with scripts/docs that pass -DBUILD_HTTP / -DBUILD_TESTING.
if(BUILD_HTTP)
  set(PP_LEDGER_BUILD_HTTP ON CACHE BOOL "Build HTTP API server (pp-http)" FORCE)
endif()
if(BUILD_TESTING)
  set(PP_LEDGER_BUILD_TESTS ON CACHE BOOL "Build the testing tree" FORCE)
endif()

if(PP_LEDGER_BUILD_TESTS)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
  enable_testing()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(PpCppCommon)
include(PpCppCrypto)
include(PpCppAmp)

set(PP_SRC_DIR "${CMAKE_SOURCE_DIR}/src")

add_subdirectory(src)

include(PpLedgerTargets)
pp_ledger_define_aliases()
