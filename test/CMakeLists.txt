# Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Ouroboros Consensus
add_executable(test_ouroboros_consensus
    test_ouroboros_consensus.cpp
)

target_link_libraries(test_ouroboros_consensus PRIVATE
    ppledger_lib
    consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_consensus)

# Test for Ouroboros Epoch Manager
add_executable(test_ouroboros_epoch
    test_ouroboros_epoch.cpp
)

target_link_libraries(test_ouroboros_epoch PRIVATE
    ppledger_lib
    consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_epoch)

# Test for Ouroboros VRF and Slot Leader Selection
add_executable(test_ouroboros_vrf
    test_ouroboros_vrf.cpp
)

target_link_libraries(test_ouroboros_vrf PRIVATE
    ppledger_lib
    consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_vrf)

# Test for Logger
add_executable(test_logger
    test_logger.cpp
)

target_link_libraries(test_logger PRIVATE
    ppledger_lib
    GTest::gtest_main
)

gtest_discover_tests(test_logger)

# Test for ResultOrError
add_executable(test_result
    test_result.cpp
)

target_link_libraries(test_result PRIVATE
    ppledger_lib
    GTest::gtest_main
)

gtest_discover_tests(test_result)

# Test for Module base class
add_executable(test_module
    test_module.cpp
)

target_link_libraries(test_module PRIVATE
    ppledger_lib
    ppledger_client
    ppledger_server
    crypto
    GTest::gtest_main
)

gtest_discover_tests(test_module)

# Test for Wallet
add_executable(test_wallet
    test_wallet.cpp
)

target_link_libraries(test_wallet PRIVATE
    ppledger_lib
    ppledger_server
    GTest::gtest_main
)

gtest_discover_tests(test_wallet)

# Test for BlockChain
add_executable(test_blockchain
    test_blockchain.cpp
)

target_link_libraries(test_blockchain PRIVATE
    ppledger_lib
    ppledger_server
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
    ppledger_server
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
    ppledger_server
    GTest::gtest_main
)

gtest_discover_tests(test_blockfile)

# Test for BlockDir
add_executable(test_blockdir
    test_blockdir.cpp
)

target_link_libraries(test_blockdir PRIVATE
    ppledger_lib
    ppledger_server
    GTest::gtest_main
)

gtest_discover_tests(test_blockdir)

# Test for Network Fetch
add_executable(test_fetch
    test_fetch.cpp
)

target_include_directories(test_fetch PRIVATE
    ${CMAKE_SOURCE_DIR}/network
)

target_link_libraries(test_fetch PRIVATE
    ppledger_lib
    network
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_fetch)

