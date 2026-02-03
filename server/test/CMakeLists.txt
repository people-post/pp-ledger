# Server Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for AccountBuffer
add_executable(test_account_buffer
    test_account_buffer.cpp
)

target_link_libraries(test_account_buffer PRIVATE
    pp_server
    GTest::gtest_main
)

gtest_discover_tests(test_account_buffer)
