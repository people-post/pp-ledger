include(GoogleTest)

add_executable(test_account_buffer test_account_buffer.cpp)
target_link_libraries(test_account_buffer PRIVATE pp_chain GTest::gtest_main)
gtest_discover_tests(test_account_buffer)

add_executable(test_account_state_tree test_account_state_tree.cpp)
target_link_libraries(test_account_state_tree PRIVATE pp_chain GTest::gtest_main)
gtest_discover_tests(test_account_state_tree)

add_executable(test_chain test_chain.cpp)
target_link_libraries(test_chain PRIVATE pp_chain pp_testing GTest::gtest_main)
gtest_discover_tests(test_chain)

add_executable(test_tx_idempotency test_tx_idempotency.cpp)
target_link_libraries(test_tx_idempotency PRIVATE pp_chain GTest::gtest_main)
gtest_discover_tests(test_tx_idempotency)

# Ledger scale (load-resilience lr9): skipped unless PP_LEDGER_SCALE=1 (--suite scale).
add_executable(test_ledger_scale test_ledger_scale.cpp)
target_link_libraries(test_ledger_scale PRIVATE pp_chain pp_testing GTest::gtest_main)
gtest_discover_tests(test_ledger_scale)
