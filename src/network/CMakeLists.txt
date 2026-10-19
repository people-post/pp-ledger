cmake_minimum_required(VERSION 3.14)

project(network VERSION 1.0.0 LANGUAGES CXX)

add_library(pp_network INTERFACE)

target_include_directories(pp_network INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(pp_network
    INTERFACE
        pp_ledger_common
)

add_library(pp_ledger_amp STATIC
    AmpHost.cpp
    AmpLedgerServer.cpp
    HostedAmpRuntime.cpp
    InFlightLimit.cpp
    LedgerAmpRuntime.cpp
    ServerAmpSupport.cpp
    amp/AmpIdentity.cpp
    NetworkTuning.cpp
)

target_include_directories(pp_ledger_amp PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(pp_ledger_amp PUBLIC
    pp_amp_link
    pp_ledger_common
    pp_crypto
    mldsa_native
)

target_sources(pp_ledger_amp PRIVATE
    ${PP_SRC_DIR}/client/AmpLedgerTransport.cpp
)

if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
    add_subdirectory(amp/test)
endif()
