# Agent Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Agent (Wallet & Transactions)
add_executable(test_agent
    test_agent.cpp
)

target_link_libraries(test_agent PRIVATE
    pp_lib
    pp_ledger
    pp_server
    OpenSSL::Crypto
    GTest::gtest_main
)

gtest_discover_tests(test_agent)
