cmake_minimum_required(VERSION 3.14)

project(network VERSION 1.0.0 LANGUAGES CXX)

add_library(pp_network STATIC Types.hpp)

target_include_directories(pp_network
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PP_SRC_DIR}
)

target_link_libraries(pp_network
    PUBLIC
        pp_ledger_common
)

set_target_properties(pp_network PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

add_library(pp_ledger_amp STATIC
    AmpLedgerServer.cpp
    LedgerAmpRuntime.cpp
    ServerAmpSupport.cpp
    amp/LedgerPeerId.cpp
    amp/AmpIdentity.cpp
)

target_include_directories(pp_ledger_amp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PP_SRC_DIR}
    ${PP_SRC_DIR}/client
)

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
    add_subdirectory(amp/test)
endif()
