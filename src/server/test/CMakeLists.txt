include(GoogleTest)

add_executable(test_beacon_server test_beacon_server.cpp)
target_link_libraries(test_beacon_server PRIVATE pp_server pp_testing GTest::gtest_main)
gtest_discover_tests(test_beacon_server)

add_executable(test_server_thread test_server_thread.cpp)
target_link_libraries(test_server_thread PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_server_thread)

add_executable(test_miner_pool test_miner_pool.cpp)
target_link_libraries(test_miner_pool PRIVATE pp_server pp_testing GTest::gtest_main)
gtest_discover_tests(test_miner_pool)

add_executable(test_block_sync test_block_sync.cpp)
target_link_libraries(test_block_sync PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_block_sync)

add_executable(test_signed_tip test_signed_tip.cpp)
target_link_libraries(test_signed_tip PRIVATE pp_server pp_testing GTest::gtest_main)
gtest_discover_tests(test_signed_tip)

add_executable(test_rewind test_rewind.cpp)
target_link_libraries(test_rewind PRIVATE pp_server pp_testing GTest::gtest_main)
gtest_discover_tests(test_rewind)

add_executable(test_upstream_set test_upstream_set.cpp)
target_link_libraries(test_upstream_set PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_upstream_set)

add_executable(test_clock_calibration test_clock_calibration.cpp)
target_link_libraries(test_clock_calibration PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_clock_calibration)

add_executable(test_env_config test_env_config.cpp)
target_link_libraries(test_env_config PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_env_config)

add_executable(test_node_health test_node_health.cpp)
target_link_libraries(test_node_health PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_node_health)

add_executable(test_load_generator test_load_generator.cpp)
target_link_libraries(test_load_generator PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_load_generator)

add_executable(test_overload test_overload.cpp)
target_link_libraries(test_overload PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_overload)

add_executable(test_hosted_amp test_hosted_amp.cpp)
target_link_libraries(test_hosted_amp PRIVATE pp_server pp_testing GTest::gtest_main)
gtest_discover_tests(test_hosted_amp)

add_executable(test_role_runner test_role_runner.cpp)
target_link_libraries(test_role_runner PRIVATE pp_server pp_testing GTest::gtest_main)
# A broken stop hangs rather than fails (the run cannot be ended from the test): a timeout names it.
gtest_discover_tests(test_role_runner PROPERTIES TIMEOUT 60)
