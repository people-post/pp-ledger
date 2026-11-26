# Root CMake options and library layout for pp-ledger.

cmake_minimum_required(VERSION 3.24)

project(pp-ledger VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Embedded in another project (FetchContent / add_subdirectory): libraries only, by default.
option(PP_LEDGER_BUILD_APPS "Build pp-beacon, pp-miner, pp-client, pp-relay" ${PROJECT_IS_TOP_LEVEL})
option(PP_LEDGER_BUILD_HTTP "Build HTTP API server (pp-http)" OFF)
option(PP_LEDGER_BUILD_TESTS "Build the testing tree" OFF)
# Compiler warnings fail the build by default (CI). An embedder or an older compiler with false positives (GCC 12 on
# Debian 12, for one) turns it off; pp-browser and pp-ledger-apps pass their own switch down.
option(PP_LEDGER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for pp-ledger's targets" ON)

if(PP_LEDGER_BUILD_TESTS)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  if(NOT TARGET gtest)
    add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
  endif()
  enable_testing()
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(PpCppCommon)
include(PpCppCrypto)
include(PpCppAmp)

set(PP_SRC_DIR "${PROJECT_SOURCE_DIR}/src")

add_subdirectory(src)

include(PpLedgerTargets)
pp_ledger_define_aliases()
