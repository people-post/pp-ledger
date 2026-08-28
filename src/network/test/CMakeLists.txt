# Network Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Network Fetch
add_executable(test_fetch
    test_fetch.cpp
)

target_include_directories(test_fetch PRIVATE
    ${PP_SRC_DIR}/network
)

target_link_libraries(test_fetch PRIVATE
    pp_ledger_common
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
    ${PP_SRC_DIR}/network
)

target_link_libraries(test_tcp PRIVATE
    pp_ledger_common
    pp_network
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_tcp)

# Test for BulkWriter
add_executable(test_bulkwriter
    test_bulkwriter.cpp
)

target_include_directories(test_bulkwriter PRIVATE
    ${PP_SRC_DIR}/network
)

target_link_libraries(test_bulkwriter PRIVATE
    pp_ledger_common
    pp_network
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_bulkwriter)

add_executable(test_ledger_frame_codec
    test_ledger_frame_codec.cpp
)

target_include_directories(test_ledger_frame_codec PRIVATE
    ${PP_SRC_DIR}/network
)

target_link_libraries(test_ledger_frame_codec PRIVATE
    pp_ledger_common
    pp_network
    GTest::gtest_main
)

gtest_discover_tests(test_ledger_frame_codec)

add_executable(test_fetch_security
    test_fetch_security.cpp
)

target_include_directories(test_fetch_security PRIVATE
    ${PP_SRC_DIR}/network
)

target_link_libraries(test_fetch_security PRIVATE
    pp_ledger_common
    pp_network
    GTest::gtest_main
)

gtest_discover_tests(test_fetch_security)
