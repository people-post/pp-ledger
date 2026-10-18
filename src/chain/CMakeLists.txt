# The chain's data types (config, authority, certificates, genesis meta): no network code, so admin
# tools can read and build them (operation files).
add_library(pp_chain_types STATIC
    Types.cpp
    Types.h
)
target_include_directories(pp_chain_types PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PP_SRC_DIR})
target_link_libraries(pp_chain_types PUBLIC pp_ledger_common pp_ledger)

# Chain state: AccountBuffer, block/tx validation helpers, Chain orchestration.
add_library(pp_chain STATIC
    AccountBuffer.cpp
    AccountBuffer.h
    AccountStateTree.cpp
    AccountStateTree.h
    BlockValidation.cpp
    BlockValidation.h
    Chain.cpp
    Chain.h
    RecordHandler.cpp
    RecordHandler.h
    ErrorCodes.h
    BlockAdmission.h
    TxContext.h
    TxError.h
    TxFees.cpp
    TxFees.h
    TxTyped.h
    TxSignatures.cpp
    TxSignatures.h
    TxIdempotency.cpp
    TxIdempotency.h
    TxLedgerMeta.cpp
    TxLedgerMeta.h
    AccountPolicy.cpp
    AccountPolicy.h
    ITxHandler.h
    UserAccountUpsertBase.cpp
    UserAccountUpsertBase.h
    Genesis.cpp
    Genesis.h
    GenesisTxHandler.cpp
    GenesisTxHandler.h
    ConfigPolicy.cpp
    StateSnapshot.cpp
    ImportTxHandler.cpp
    PredecessorCheck.cpp
    ConfigPolicy.h
    AuthorityTrail.cpp
    ClientVerify.cpp
    AuthorityTrail.h
    BeaconCertTxHandler.cpp
    BeaconCertTxHandler.h
    RecoveryTxHandler.cpp
    RecoveryTxHandler.h
    ConfigTxHandler.cpp
    ConfigTxHandler.h
    DefaultTxHandler.cpp
    DefaultTxHandler.h
    EndUserTxHandler.cpp
    EndUserTxHandler.h
    MinerTxHandler.cpp
    MinerTxHandler.h
    MinerLiveness.cpp
    MinerLiveness.h
    RenewalTxHandler.cpp
    RenewalTxHandler.h
    NewUserTxHandler.cpp
    NewUserTxHandler.h
    UserUpdateTxHandler.cpp
    UserUpdateTxHandler.h
)

target_include_directories(pp_chain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PP_SRC_DIR}/network
)

target_link_libraries(pp_chain PUBLIC
    pp_chain_types
    pp_ledger_common
    pp_ledger
    pp_client
    pp_consensus
    pp_network
)

message(STATUS "Chain library configured")

if(PP_LEDGER_BUILD_TESTS)
  add_subdirectory(test)
endif()
