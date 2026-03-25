# Server library
add_library(pp_server STATIC
    Server.cpp
    Server.h
    BeaconServer.cpp
    BeaconServer.h
    Beacon.cpp
    Beacon.h
    MinerServer.cpp
    MinerServer.h
    Miner.cpp
    Miner.h
    RelayServer.cpp
    RelayServer.h
    Relay.cpp
    Relay.h
)

target_include_directories(pp_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/network
)

target_link_libraries(pp_server PUBLIC
    pp_chain
    pp_lib
    pp_ledger
    pp_client
    pp_consensus
    pp_network
)

message(STATUS "Server configured with TCP network support")
