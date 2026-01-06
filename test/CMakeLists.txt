# Test executables

# Test for Logger
add_executable(test_logger
    test_logger.cpp
)

target_link_libraries(test_logger PRIVATE
    ppledger_lib
)

add_test(NAME test_logger COMMAND test_logger)

# Test for Ledger
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

# Test for Wallet
add_executable(test_wallet
    test_wallet.cpp
)

target_link_libraries(test_wallet PRIVATE
    ppledger_lib
    ppledger_server
)

add_test(NAME test_wallet COMMAND test_wallet)
