# Ledger Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Wallet
add_executable(test_wallet
    test_wallet.cpp
)

target_link_libraries(test_wallet PRIVATE
    ppledger_lib
    ppledger_ledger
    GTest::gtest_main
)

gtest_discover_tests(test_wallet)

# Test for BlockChain
add_executable(test_blockchain
    test_blockchain.cpp
)

target_link_libraries(test_blockchain PRIVATE
    ppledger_lib
    ppledger_ledger
    crypto
    GTest::gtest_main
)

gtest_discover_tests(test_blockchain)

# Test for Ledger (Wallet & Transactions)
add_executable(test_ledger
    test_ledger.cpp
)

target_link_libraries(test_ledger PRIVATE
    ppledger_lib
    ppledger_ledger
    crypto
    GTest::gtest_main
)

gtest_discover_tests(test_ledger)

# Test for BlockFile
add_executable(test_blockfile
    test_blockfile.cpp
)

target_link_libraries(test_blockfile PRIVATE
    ppledger_lib
    ppledger_ledger
    GTest::gtest_main
)

gtest_discover_tests(test_blockfile)
