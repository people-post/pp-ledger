cmake_minimum_required(VERSION 3.15)

project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_NODE_ADDON "Build Node.js N-API addon" OFF)
option(BUILD_HTTP "Build HTTP API server (pp-http)" OFF)

if(BUILD_NODE_ADDON)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Treat warnings as errors
add_compile_options(-Werror)

# Find Libsodium (required for cryptographic operations)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

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

# Add subdirectories for each component
add_subdirectory(lib)
add_subdirectory(consensus)
add_subdirectory(network)
add_subdirectory(client)
add_subdirectory(ledger)
add_subdirectory(chain)
add_subdirectory(server)
if(BUILD_HTTP)
  add_subdirectory(lib/http)
endif()
add_subdirectory(app)

if(BUILD_NODE_ADDON)
  add_subdirectory(node-addon)
endif()
