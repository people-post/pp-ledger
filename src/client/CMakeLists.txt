# Client library
add_library(pp_client STATIC
    Client.cpp
    AccountAttachment.cpp
    HealthMetrics.cpp
    LoadGenerator.cpp
    AmpLedgerTransport.cpp
    InProcessLedgerTransport.cpp
)

target_include_directories(pp_client PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(pp_client PUBLIC
    pp_ledger_common
    pp_ledger
    pp_chain_types
    pp_network
    pp_ledger_amp
)
