# Server Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for BlockChain
add_executable(test_blockchain
    test_blockchain.cpp
)

target_link_libraries(test_blockchain PRIVATE
    pp_server
    GTest::gtest_main
)

gtest_discover_tests(test_blockchain)

# Test for AccountBuffer
add_executable(test_account_buffer
    test_account_buffer.cpp
)

target_link_libraries(test_account_buffer PRIVATE
    pp_server
    GTest::gtest_main
)

gtest_discover_tests(test_account_buffer)
