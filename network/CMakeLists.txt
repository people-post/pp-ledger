cmake_minimum_required(VERSION 3.14)

# Network Library
project(network VERSION 1.0.0 LANGUAGES CXX)

# Network library requires C++20 for cpp-libp2p compatibility (uses std::span)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(NETWORK_SOURCES
    FetchClient.cpp
    FetchServer.cpp
)

# Header files
set(NETWORK_HEADERS
    FetchClient.h
    FetchServer.h
)

# Create the library
add_library(network STATIC ${NETWORK_SOURCES} ${NETWORK_HEADERS})

# Include directories
target_include_directories(network
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/lib
)

# Link with lib and libp2p
target_link_libraries(network
    PUBLIC
        ppledger_lib
        p2p::p2p
)

# Set properties
set_target_properties(network PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${NETWORK_HEADERS}"
)

# Installation rules
install(TARGETS network
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include/network
)
