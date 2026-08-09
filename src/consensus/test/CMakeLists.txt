# Consensus Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for Ouroboros Consensus
add_executable(test_ouroboros_consensus
    test_ouroboros_consensus.cpp
)

target_link_libraries(test_ouroboros_consensus PRIVATE
    pp_lib
    pp_consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_consensus)

# Test for Ouroboros Epoch Manager
add_executable(test_ouroboros_epoch
    test_ouroboros_epoch.cpp
)

target_link_libraries(test_ouroboros_epoch PRIVATE
    pp_lib
    pp_consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_epoch)

# Test for Ouroboros VRF and Slot Leader Selection
add_executable(test_ouroboros_vrf
    test_ouroboros_vrf.cpp
)

target_link_libraries(test_ouroboros_vrf PRIVATE
    pp_lib
    pp_consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_vrf)
