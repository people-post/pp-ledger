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
    ${PP_SRC_DIR}/network
)

target_link_libraries(pp_server PUBLIC
    pp_chain
    pp_ledger_common
    pp_ledger
    pp_client
    pp_consensus
    pp_network
)

if(PP_LEDGER_AMP)
  target_compile_definitions(pp_server PUBLIC PP_LEDGER_HAS_AMP)
  target_link_libraries(pp_server PUBLIC pp_ledger_amp)
endif()

message(STATUS "Server configured with TCP network support")
