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
