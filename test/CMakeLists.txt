# Test executables

# Test for Logger
add_executable(test_logger
    test_logger.cpp
)

target_link_libraries(test_logger PRIVATE
    ppledger_lib
)

add_test(NAME test_logger COMMAND test_logger)

# Test for BlockChain
add_executable(test_blockchain
    test_blockchain.cpp
)

target_link_libraries(test_blockchain PRIVATE
    ppledger_lib
    ppledger_server
    crypto
)

add_test(NAME test_blockchain COMMAND test_blockchain)

# Test for Ledger (Wallet & Transactions)
add_executable(test_ledger
    test_ledger.cpp
)

target_link_libraries(test_ledger PRIVATE
    ppledger_lib
    ppledger_server
    crypto
)

add_test(NAME test_ledger COMMAND test_ledger)

# Test for ResultOrError
add_executable(test_result
    test_result.cpp
)

target_link_libraries(test_result PRIVATE
    ppledger_lib
)

add_test(NAME test_result COMMAND test_result)

# Test for Module base class
add_executable(test_module
    test_module.cpp
)

target_link_libraries(test_module PRIVATE
    ppledger_lib
    ppledger_client
    ppledger_server
    crypto
)

add_test(NAME test_module COMMAND test_module)# Test for Wallet
add_executable(test_wallet
    test_wallet.cpp
)

target_link_libraries(test_wallet PRIVATE
    ppledger_lib
    ppledger_server
)

add_test(NAME test_wallet COMMAND test_wallet)
