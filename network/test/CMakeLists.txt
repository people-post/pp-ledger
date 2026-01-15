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
    pp_lib
    pp_network
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_fetch)

# Test for TCP Client/Server/Connection
add_executable(test_tcp
    test_tcp.cpp
)

target_include_directories(test_tcp PRIVATE
    ${CMAKE_SOURCE_DIR}/network
)

target_link_libraries(test_tcp PRIVATE
    pp_lib
    pp_network
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_tcp)
