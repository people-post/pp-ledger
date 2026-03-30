# Chain state: AccountBuffer, block/tx validation helpers, Chain orchestration.
add_library(pp_chain STATIC
    AccountBuffer.cpp
    AccountBuffer.h
    BlockValidation.cpp
    BlockValidation.h
    Chain.cpp
    Chain.h
    RecordHandler.cpp
    RecordHandler.h
    Types.cpp
    Types.h
    ErrorCodes.h
    TxContext.h
    TxError.h
    TxFees.cpp
    TxFees.h
    TxSignatures.cpp
    TxSignatures.h
    TxIdempotency.cpp
    TxIdempotency.h
    TxLedgerMeta.cpp
    TxLedgerMeta.h
    ITxHandler.h
    UserAccountUpsertBase.cpp
    UserAccountUpsertBase.h
    GenesisTxHandler.cpp
    GenesisTxHandler.h
    ConfigTxHandler.cpp
    ConfigTxHandler.h
    DefaultTxHandler.cpp
    DefaultTxHandler.h
    EndUserTxHandler.cpp
    EndUserTxHandler.h
    RenewalTxHandler.cpp
    RenewalTxHandler.h
    NewUserTxHandler.cpp
    NewUserTxHandler.h
    UserUpdateTxHandler.cpp
    UserUpdateTxHandler.h
)

target_include_directories(pp_chain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/network
)

target_link_libraries(pp_chain PUBLIC
    pp_lib
    pp_ledger
    pp_client
    pp_consensus
    pp_network
)

message(STATUS "Chain library configured")

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
