# Server library
add_library(pp_server STATIC
    Server.cpp
    RoleLoop.cpp
    NodeConfig.cpp
    NodeCore.cpp
    FollowerCore.cpp
    FollowerServer.cpp
    RequestQueue.cpp
    BlockWaitList.cpp
    TxPool.cpp
    CertificateStore.cpp
    EnvConfig.cpp
    EquivocationStore.cpp
    NodeFile.cpp
    Rewind.cpp
    TipSigner.cpp
    ClockCalibration.cpp
    UpstreamSet.cpp
    UpstreamTipWatch.cpp
    BlockSync.cpp
    BeaconServer.cpp
    Beacon.cpp
    MinerServer.cpp
    Miner.cpp
    RelayServer.cpp
    RoleRunner.cpp
    Relay.cpp
)

target_include_directories(pp_server PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(pp_server PUBLIC
    pp_chain
    pp_operation
    pp_ledger_common
    pp_ledger
    pp_client
    pp_consensus
    pp_network
    pp_ledger_amp
)

message(STATUS "Server configured with AMP network support")

if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
endif()
