include(GoogleTest)

add_executable(test_network_tuning
    test_network_tuning.cpp
)

target_include_directories(test_network_tuning PRIVATE
    ${PP_SRC_DIR}/network
    ${PP_SRC_DIR}
)

target_link_libraries(test_network_tuning PRIVATE
    pp_ledger_amp
    pp_ledger_common
    GTest::gtest_main
)

gtest_discover_tests(test_network_tuning)
