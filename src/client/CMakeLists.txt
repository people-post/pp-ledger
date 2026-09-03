# Client library
add_library(pp_client STATIC
    Client.cpp
    Client.h
    AccountAttachment.cpp
    AccountAttachment.h
    ILedgerTransport.h
    AmpLedgerTransport.cpp
    AmpLedgerTransport.h
    InProcessLedgerTransport.cpp
    InProcessLedgerTransport.h
)

target_include_directories(pp_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PP_SRC_DIR}
)

target_link_libraries(pp_client PUBLIC
    pp_ledger_common
    pp_ledger
    pp_network
    pp_ledger_amp
)
