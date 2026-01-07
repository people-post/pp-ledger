cmake_minimum_required(VERSION 3.15)

project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Find system Boost
find_package(Boost REQUIRED COMPONENTS system thread random filesystem)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find cpp-libp2p (required)
# Set LIBP2P_ROOT to the installation directory
if(NOT DEFINED LIBP2P_ROOT)
    message(FATAL_ERROR "LIBP2P_ROOT must be set to the cpp-libp2p installation directory")
endif()

set(CMAKE_PREFIX_PATH "${LIBP2P_ROOT};${CMAKE_PREFIX_PATH}")

# Add libp2p include directories
include_directories(${LIBP2P_ROOT}/include)

# Add libp2p library directories
link_directories(${LIBP2P_ROOT}/lib)

# Find the main libp2p library
find_library(LIBP2P_LIBRARY NAMES p2p libp2p PATHS ${LIBP2P_ROOT}/lib NO_DEFAULT_PATH)

if(NOT LIBP2P_LIBRARY)
    message(FATAL_ERROR "cpp-libp2p not found in ${LIBP2P_ROOT}")
endif()

message(STATUS "Found cpp-libp2p: ${LIBP2P_LIBRARY}")

# Create an interface library for libp2p
add_library(p2p::p2p INTERFACE IMPORTED)
set_target_properties(p2p::p2p PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LIBP2P_ROOT}/include"
    INTERFACE_LINK_DIRECTORIES "${LIBP2P_ROOT}/lib"
)
target_link_libraries(p2p::p2p INTERFACE ${LIBP2P_LIBRARY})

# Set C++ standard to C++20 (required for libp2p)
set(CMAKE_CXX_STANDARD 20)
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
add_subdirectory(network)
add_subdirectory(client)
add_subdirectory(server)
add_subdirectory(app)

# Enable testing
enable_testing()
add_subdirectory(test)
