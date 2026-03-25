# Chain state: AccountBuffer, block/tx validation helpers, Chain orchestration.
add_library(pp_chain STATIC
    AccountBuffer.cpp
    AccountBuffer.h
    Chain.cpp
    Chain.h
    ChainTypes.cpp
    ChainTypes.h
    ChainErrorCodes.h
    ChainTransactionContext.h
    ChainTxError.h
    ChainTxFees.cpp
    ChainTxFees.h
    ChainTxSignatures.cpp
    ChainTxSignatures.h
    ChainTxIdempotency.cpp
    ChainTxIdempotency.h
    ChainTxLedgerMeta.cpp
    ChainTxLedgerMeta.h
    ITransactionHandler.h
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

