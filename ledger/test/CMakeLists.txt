# Agent Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Wallet
add_executable(test_wallet
    test_wallet.cpp
)

target_link_libraries(test_wallet PRIVATE
    pp_lib
    pp_ledger
    GTest::gtest_main
)

gtest_discover_tests(test_wallet)

# Test for BlockChain
add_executable(test_blockchain
    test_blockchain.cpp
)

target_link_libraries(test_blockchain PRIVATE
    pp_lib
    pp_ledger
    OpenSSL::Crypto
    GTest::gtest_main
)

gtest_discover_tests(test_blockchain)

# Test for FileStore
add_executable(test_blockfile
    test_blockfile.cpp
)

target_link_libraries(test_blockfile PRIVATE
    pp_lib
    pp_ledger
    GTest::gtest_main
)

gtest_discover_tests(test_blockfile)

# Test for BlockDir
add_executable(test_blockdir
    test_blockdir.cpp
)

target_link_libraries(test_blockdir PRIVATE
    pp_lib
    pp_ledger
    OpenSSL::Crypto
    GTest::gtest_main
)

gtest_discover_tests(test_blockdir)

# Test for FileDirStore
add_executable(test_filedirstore
    test_filedirstore.cpp
)

target_link_libraries(test_filedirstore PRIVATE
    pp_lib
    pp_ledger
    GTest::gtest_main
)

gtest_discover_tests(test_filedirstore)

# Test for DirDirStore
add_executable(test_dirdirstore
    test_dirdirstore.cpp
)

target_link_libraries(test_dirdirstore PRIVATE
    pp_lib
    pp_ledger
    GTest::gtest_main
)

gtest_discover_tests(test_dirdirstore)

# Test for Ledger
add_executable(test_ledger
    test_ledger.cpp
)

target_link_libraries(test_ledger PRIVATE
    pp_lib
    pp_ledger
    OpenSSL::Crypto
    GTest::gtest_main
)

gtest_discover_tests(test_ledger)
