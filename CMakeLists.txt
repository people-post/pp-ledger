cmake_minimum_required(VERSION 3.15)

project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treat warnings as errors
add_compile_options(-Werror)

# Find OpenSSL (required for ledger and some tests)
find_package(OpenSSL REQUIRED)

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Fetch nlohmann/json
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# Enable testing and set BUILD_TESTING before adding subdirectories
enable_testing()
option(BUILD_TESTING "Build the testing tree" ON)

# Add subdirectories for each component
add_subdirectory(lib)
add_subdirectory(interface)
add_subdirectory(consensus)
add_subdirectory(network)
add_subdirectory(client)
add_subdirectory(ledger)
add_subdirectory(server)
add_subdirectory(app)
