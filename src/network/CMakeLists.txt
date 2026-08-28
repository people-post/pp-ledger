cmake_minimum_required(VERSION 3.14)

project(network VERSION 1.0.0 LANGUAGES CXX C)

add_subdirectory(dht)

set(NETWORK_SOURCES
    BulkWriter.cpp
    DhtRunner.cpp
    FetchClient.cpp
    FetchServer.cpp
    LedgerFrameCodec.cpp
    TcpClient.cpp
    TcpConnection.cpp
    TcpServer.cpp
)

set(NETWORK_HEADERS
    BulkWriter.h
    DhtRunner.h
    FetchClient.h
    FetchServer.h
    LedgerFrameCodec.h
    TcpClient.h
    TcpConnection.h
    TcpServer.h
)

add_library(pp_network STATIC ${NETWORK_SOURCES} ${NETWORK_HEADERS})

target_include_directories(pp_network
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PP_SRC_DIR}
)

target_link_libraries(pp_network
    PUBLIC
        pp_ledger_common
        pp_dht
)

set_target_properties(pp_network PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${NETWORK_HEADERS}"
)

install(TARGETS pp_network
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include/network
)

if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
endif()
