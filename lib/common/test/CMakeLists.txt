# Lib Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Logger
add_executable(test_logger
    test_logger.cpp
)

target_link_libraries(test_logger PRIVATE
    pp_lib
    GTest::gtest_main
)

gtest_discover_tests(test_logger)

# Test for ResultOrError
add_executable(test_result
    test_result.cpp
)

target_link_libraries(test_result PRIVATE
    pp_lib
    GTest::gtest_main
)

gtest_discover_tests(test_result)

# Test for Archive (serialization)
add_executable(test_archive
    test_archive.cpp
)

target_link_libraries(test_archive PRIVATE
    pp_lib
    GTest::gtest_main
)

gtest_discover_tests(test_archive)

# Test for Utilities (Ed25519, etc.)
add_executable(test_utilities
    test_utilities.cpp
)

target_link_libraries(test_utilities PRIVATE
    pp_lib
    GTest::gtest_main
)

gtest_discover_tests(test_utilities)
