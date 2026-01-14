# Lib Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Logger
add_executable(test_logger
    test_logger.cpp
)

target_link_libraries(test_logger PRIVATE
    ppledger_lib
    GTest::gtest_main
)

gtest_discover_tests(test_logger)

# Test for ResultOrError
add_executable(test_result
    test_result.cpp
)

target_link_libraries(test_result PRIVATE
    ppledger_lib
    GTest::gtest_main
)

gtest_discover_tests(test_result)

# Test for Module base class
add_executable(test_module
    test_module.cpp
)

target_link_libraries(test_module PRIVATE
    ppledger_lib
    ppledger_client
    ppledger_server
    crypto
    GTest::gtest_main
)

gtest_discover_tests(test_module)

# Test for Serializer
add_executable(test_serializer
    test_serializer.cpp
)

target_link_libraries(test_serializer PRIVATE
    ppledger_lib
    GTest::gtest_main
)

gtest_discover_tests(test_serializer)
