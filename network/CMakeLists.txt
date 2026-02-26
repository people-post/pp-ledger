cmake_minimum_required(VERSION 3.14)

# Network Library (CXX + C for DHT)
project(network VERSION 1.0.0 LANGUAGES CXX C)

# DHT subdirectory (jech/dht)
add_subdirectory(dht)

# Source files
set(NETWORK_SOURCES
    BulkWriter.cpp
    FetchClient.cpp
    FetchServer.cpp
    TcpClient.cpp
    TcpConnection.cpp
    TcpServer.cpp
)

# Header files
set(NETWORK_HEADERS
    BulkWriter.h
    FetchClient.h
    FetchServer.h
    TcpClient.h
    TcpConnection.h
    TcpServer.h
)

# Create the library
add_library(pp_network STATIC ${NETWORK_SOURCES} ${NETWORK_HEADERS})

# Include directories
target_include_directories(pp_network
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/lib
)

# Link with lib (uses ResultOrError from lib) and DHT (optional peer discovery)
target_link_libraries(pp_network
    PUBLIC
        pp_lib
        pp_dht
)

# Set properties
set_target_properties(pp_network PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${NETWORK_HEADERS}"
)

# Installation rules
install(TARGETS pp_network
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include/network
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
