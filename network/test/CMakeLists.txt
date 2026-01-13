# Network Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Network Fetch
add_executable(test_fetch
    test_fetch.cpp
)

target_include_directories(test_fetch PRIVATE
    ${CMAKE_SOURCE_DIR}/network
)

target_link_libraries(test_fetch PRIVATE
    ppledger_lib
    network
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_fetch)
