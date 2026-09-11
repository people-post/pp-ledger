# Consensus Test executables using Google Test

# Include GoogleTest
include(GoogleTest)

# Test for SlotCommittee (live path)
add_executable(test_slot_committee
    test_slot_committee.cpp
)

target_link_libraries(test_slot_committee PRIVATE
    pp_ledger_common
    pp_consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_slot_committee)

# EpochManager demo (not wired into Chain)
add_executable(test_ouroboros_epoch
    test_ouroboros_epoch.cpp
)

target_link_libraries(test_ouroboros_epoch PRIVATE
    pp_ledger_common
    pp_consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_epoch)

# VRF SlotLeaderSelection demo (not wired into Chain)
add_executable(test_ouroboros_vrf
    test_ouroboros_vrf.cpp
)

target_link_libraries(test_ouroboros_vrf PRIVATE
    pp_ledger_common
    pp_consensus
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(test_ouroboros_vrf)
