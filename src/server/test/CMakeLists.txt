include(GoogleTest)

add_executable(test_beacon_server test_beacon_server.cpp)
target_link_libraries(test_beacon_server PRIVATE pp_server GTest::gtest_main)
gtest_discover_tests(test_beacon_server)
