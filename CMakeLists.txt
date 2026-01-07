cmake_minimum_required(VERSION 3.15)

project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Find system Boost
find_package(Boost REQUIRED COMPONENTS system thread random filesystem)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Optional: Find cpp-libp2p if available
# Can be enabled with -DUSE_LIBP2P=ON and -DLIBP2P_ROOT=/path/to/libp2p
option(USE_LIBP2P "Enable cpp-libp2p support" OFF)

if(USE_LIBP2P)
    if(DEFINED LIBP2P_ROOT)
        set(CMAKE_PREFIX_PATH "${LIBP2P_ROOT};${CMAKE_PREFIX_PATH}")
        
        # Add libp2p include directories
        include_directories(${LIBP2P_ROOT}/include)
        
        # Add libp2p library directories
        link_directories(${LIBP2P_ROOT}/lib)
        
        # Find the main libp2p library
        find_library(LIBP2P_LIBRARY NAMES p2p libp2p PATHS ${LIBP2P_ROOT}/lib NO_DEFAULT_PATH)
        
        if(LIBP2P_LIBRARY)
            message(STATUS "Found cpp-libp2p: ${LIBP2P_LIBRARY}")
            set(LIBP2P_FOUND TRUE)
            
            # Create an interface library for libp2p
            add_library(p2p::p2p INTERFACE IMPORTED)
            set_target_properties(p2p::p2p PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${LIBP2P_ROOT}/include"
                INTERFACE_LINK_DIRECTORIES "${LIBP2P_ROOT}/lib"
            )
            target_link_libraries(p2p::p2p INTERFACE ${LIBP2P_LIBRARY})
        else()
            message(WARNING "cpp-libp2p not found in ${LIBP2P_ROOT}")
            set(LIBP2P_FOUND FALSE)
        endif()
    else()
        message(WARNING "USE_LIBP2P is ON but LIBP2P_ROOT is not set")
        set(LIBP2P_FOUND FALSE)
    endif()
else()
    message(STATUS "cpp-libp2p support is disabled. Network library will not be built.")
    set(LIBP2P_FOUND FALSE)
endif()

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
# Network library requires cpp-libp2p - only build if available
if(LIBP2P_FOUND)
    add_subdirectory(network)
    message(STATUS "Building network library with cpp-libp2p support")
else()
    message(STATUS "Skipping network library (cpp-libp2p not available)")
endif()
add_subdirectory(client)
add_subdirectory(server)
add_subdirectory(app)

# Enable testing
enable_testing()
add_subdirectory(test)
