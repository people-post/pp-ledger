cmake_minimum_required(VERSION 3.15)

project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Find system Boost
find_package(Boost REQUIRED COMPONENTS system thread random filesystem)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Note: cpp-libp2p can be built from source in the libp2p/ directory
# and linked manually when needed

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treat warnings as errors
add_compile_options(-Werror)

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

# Add subdirectories for each component
add_subdirectory(lib)
add_subdirectory(consensus)
# Network library requires cpp-libp2p - commented out until libp2p is available
# add_subdirectory(network)
add_subdirectory(client)
add_subdirectory(server)
add_subdirectory(app)

# Enable testing
enable_testing()
add_subdirectory(test)
