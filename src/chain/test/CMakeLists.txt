include(GoogleTest)

add_executable(test_account_buffer test_account_buffer.cpp)
target_link_libraries(test_account_buffer PRIVATE pp_chain GTest::gtest_main)
gtest_discover_tests(test_account_buffer)

add_executable(test_chain test_chain.cpp)
target_link_libraries(test_chain PRIVATE pp_chain GTest::gtest_main)
gtest_discover_tests(test_chain)
