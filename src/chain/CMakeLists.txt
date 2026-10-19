# The chain's data types (config, authority, certificates, genesis meta): no network code, so admin
# tools can read and build them (operation files).
add_library(pp_chain_types STATIC
    Types.cpp
)
target_include_directories(pp_chain_types PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(pp_chain_types PUBLIC pp_ledger_common pp_ledger)

# Chain state: AccountBuffer, block/tx validation helpers, Chain orchestration.
add_library(pp_chain STATIC
    AccountBuffer.cpp
    Domains.cpp
    AccountStateTree.cpp
    BlockValidation.cpp
    Chain.cpp
    RecordHandler.cpp
    TxFees.cpp
    TxSignatures.cpp
    TxIdempotency.cpp
    TxLedgerMeta.cpp
    AccountPolicy.cpp
    UserAccountUpsertBase.cpp
    Genesis.cpp
    GenesisTxHandler.cpp
    ConfigPolicy.cpp
    StateSnapshot.cpp
    ImportTxHandler.cpp
    PredecessorCheck.cpp
    AccountRecoveryTxHandler.cpp
    DomainAssignTxHandler.cpp
    AuthorityTrail.cpp
    ClientVerify.cpp
    BeaconCertTxHandler.cpp
    RecoveryTxHandler.cpp
    ConfigTxHandler.cpp
    DefaultTxHandler.cpp
    EndUserTxHandler.cpp
    MinerTxHandler.cpp
    MinerLiveness.cpp
    RenewalTxHandler.cpp
    NewUserTxHandler.cpp
    UserUpdateTxHandler.cpp
)

target_include_directories(pp_chain PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

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
