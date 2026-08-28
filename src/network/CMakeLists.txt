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

if(WIN32)
  list(APPEND NETWORK_SOURCES
    platform/SocketOps_Win32.cpp
    platform/PollWait_Win32.cpp
    platform/IoMultiplexer_Wsapoll.cpp
    platform/HostAddress_Win32.cpp
  )
elseif(APPLE)
  list(APPEND NETWORK_SOURCES
    platform/SocketOps_Posix.cpp
    platform/PollWait_Posix.cpp
    platform/IoMultiplexer_Kqueue.cpp
    platform/HostAddress_Posix.cpp
  )
else()
  list(APPEND NETWORK_SOURCES
    platform/SocketOps_Posix.cpp
    platform/PollWait_Posix.cpp
    platform/IoMultiplexer_Epoll.cpp
    platform/HostAddress_Posix.cpp
  )
endif()

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

if(WIN32)
  target_link_libraries(pp_network PUBLIC ws2_32 iphlpapi)
  target_compile_definitions(pp_network PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

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
