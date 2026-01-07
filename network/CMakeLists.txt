cmake_minimum_required(VERSION 3.14)

# Network Library
project(network VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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

# Link with lib and libp2p (if available)
target_link_libraries(network
    PUBLIC
        ppledger_lib
)

# Link libp2p if it was found
if(TARGET p2p::p2p)
    target_link_libraries(network PUBLIC p2p::p2p)
    message(STATUS "Network library configured with cpp-libp2p")
else()
    message(WARNING "Network library configured WITHOUT cpp-libp2p - libp2p features will not be available")
endif()

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
