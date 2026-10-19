include(GoogleTest)

add_executable(test_amp_ledger_rpc
    test_amp_ledger_rpc.cpp
)


target_link_libraries(test_amp_ledger_rpc PRIVATE
    pp_ledger_amp
    pp_client
    pp_ledger_common
    pp_crypto
    GTest::gtest_main
)

gtest_discover_tests(test_amp_ledger_rpc)
