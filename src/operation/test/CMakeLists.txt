include(GoogleTest)

add_executable(test_operation test_operation.cpp)
target_link_libraries(test_operation PRIVATE pp_operation GTest::gtest_main)
gtest_discover_tests(test_operation)
