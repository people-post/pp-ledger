include(GoogleTest)

add_executable(test_gateway test_gateway.cpp)
target_link_libraries(test_gateway PRIVATE pp_gateway pp_testing GTest::gtest_main)
gtest_discover_tests(test_gateway)
