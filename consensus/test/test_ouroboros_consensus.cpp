#include "Ouroboros.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <set>

using namespace pp::consensus;
using ::testing::Ge;
using ::testing::Le;
using ::testing::AnyOf;
using ::testing::Eq;

class OuroborosTest : public ::testing::Test {
protected:
    void SetUp() override {
        consensus = std::make_unique<Ouroboros>();
        consensus->init({
            .genesisTime = 0,
            .timeOffset = 0,
            .slotDuration = 5,
            .slotsPerEpoch = 10,
        });
    }

    void TearDown() override {
        consensus.reset();
    }

    std::unique_ptr<Ouroboros> consensus;
};

TEST_F(OuroborosTest, CreatesWithCorrectConfiguration) {
    EXPECT_EQ(consensus->getConfig().slotDuration, 5);
    EXPECT_EQ(consensus->getConfig().slotsPerEpoch, 10);
}

TEST_F(OuroborosTest, RegistersStakeholders) {
    consensus->setStakeholders({{1, 1000}, {2, 2000}, {3, 500}, {4, 1500}});
    
    EXPECT_EQ(consensus->getStakeholderCount(), 4);
    EXPECT_EQ(consensus->getTotalStake(), 5000);
}

TEST_F(OuroborosTest, AllowsZeroStake) {
    consensus->setStakeholders({{1, 1000}, {2, 2000}, {100, 0}});
    
    EXPECT_EQ(consensus->getStakeholderCount(), 3);
    EXPECT_EQ(consensus->getTotalStake(), 3000);
    EXPECT_EQ(consensus->getStake(100), 0);
}

TEST_F(OuroborosTest, CalculatesSlotAndEpoch) {
    uint64_t currentSlot = consensus->getCurrentSlot();
    uint64_t currentEpoch = consensus->getCurrentEpoch();
    uint64_t slotInEpoch = consensus->getSlotInEpoch(currentSlot);
    
    EXPECT_LT(slotInEpoch, 10);
    EXPECT_EQ(currentEpoch, currentSlot / 10);
}

TEST_F(OuroborosTest, SelectsSlotLeadersDeterministically) {
    consensus->setStakeholders({{1, 1000}, {2, 2000}, {3, 500}, {4, 1500}});
    
    uint64_t currentSlot = consensus->getCurrentSlot();
    
    // Select leaders for 5 consecutive slots
    std::vector<uint64_t> leaders;
    for (uint64_t i = 0; i < 5; i++) {
        uint64_t slot = currentSlot + i;
        auto leaderResult = consensus->getSlotLeader(slot);
        
        ASSERT_TRUE(leaderResult.isOk());
        uint64_t leader = leaderResult.value();
        leaders.push_back(leader);
        
        // Verify leader is one of our stakeholders
        EXPECT_THAT(leader, AnyOf(Eq(1), Eq(2), Eq(3), Eq(4)));
    }
    
    // Verify determinism: same slot always returns same leader
    for (uint64_t i = 0; i < 5; i++) {
        uint64_t slot = currentSlot + i;
        auto leaderResult = consensus->getSlotLeader(slot);
        ASSERT_TRUE(leaderResult.isOk());
        EXPECT_EQ(leaderResult.value(), leaders[i]);
        EXPECT_THAT(leaderResult.value(), AnyOf(Eq(1), Eq(2), Eq(3), Eq(4)));
    }
}

TEST_F(OuroborosTest, VerifiesSlotLeadership) {
    consensus->setStakeholders({{1, 1000}, {2, 2000}});
    
    uint64_t currentSlot = consensus->getCurrentSlot();
    auto leaderResult = consensus->getSlotLeader(currentSlot);
    
    ASSERT_TRUE(leaderResult.isOk());
    uint64_t currentLeader = leaderResult.value();
    
    EXPECT_TRUE(consensus->isSlotLeader(currentSlot, currentLeader));
    
    // Check non-leader
    uint64_t nonLeader = (currentLeader == 1) ? 2 : 1;
    EXPECT_FALSE(consensus->isSlotLeader(currentSlot, nonLeader));
}

TEST_F(OuroborosTest, SetStakeholdersOverwritesPrevious) {
    consensus->setStakeholders({{1, 1000}, {2, 2000}});
    EXPECT_EQ(consensus->getTotalStake(), 3000);
    
    consensus->setStakeholders({{1, 1500}, {2, 2000}});
    EXPECT_EQ(consensus->getTotalStake(), 3500);
    EXPECT_EQ(consensus->getStake(1), 1500);
}

TEST_F(OuroborosTest, SetStakeholdersReplacesAll) {
    consensus->setStakeholders({{1, 1000}, {2, 2000}});
    EXPECT_EQ(consensus->getStakeholderCount(), 2);
    
    consensus->setStakeholders({{10, 500}, {20, 500}, {30, 500}});
    EXPECT_EQ(consensus->getStakeholderCount(), 3);
    EXPECT_EQ(consensus->getTotalStake(), 1500);
    EXPECT_EQ(consensus->getStake(1), 0);
}

TEST_F(OuroborosTest, SetStakeholdersCanShrink) {
    consensus->setStakeholders({{1, 1000}, {2, 2000}, {3, 500}});
    EXPECT_EQ(consensus->getStakeholderCount(), 3);
    
    consensus->setStakeholders({{1, 1000}, {2, 2000}});
    EXPECT_EQ(consensus->getStakeholderCount(), 2);
    EXPECT_EQ(consensus->getStake(3), 0);
}

TEST_F(OuroborosTest, ReturnsAllStakeholders) {
    consensus->setStakeholders({{1, 1500}, {2, 2000}, {3, 1500}});
    
    auto stakeholders = consensus->getStakeholders();
    
    EXPECT_EQ(stakeholders.size(), 3);
    
    // Verify all stakeholders are present
    std::set<uint64_t> ids;
    for (const auto& sh : stakeholders) {
        ids.insert(sh.id);
    }
    
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(2));
    EXPECT_TRUE(ids.count(3));
}

TEST_F(OuroborosTest, UpdatesSlotDuration) {
    Ouroboros::Config cfg = consensus->getConfig();
    cfg.slotDuration = 10;
    consensus->init(cfg);
    EXPECT_EQ(consensus->getConfig().slotDuration, 10);
}

TEST_F(OuroborosTest, UpdatesSlotsPerEpoch) {
    Ouroboros::Config cfg = consensus->getConfig();
    cfg.slotsPerEpoch = 20;
    consensus->init(cfg);
    EXPECT_EQ(consensus->getConfig().slotsPerEpoch, 20);
}

TEST_F(OuroborosTest, SetsGenesisTime) {
    int64_t genesisTime = 1234567890;
    Ouroboros::Config cfg = consensus->getConfig();
    cfg.genesisTime = genesisTime;
    consensus->init(cfg);
    EXPECT_EQ(consensus->getConfig().genesisTime, genesisTime);
}

TEST_F(OuroborosTest, SetsTimeOffset) {
    Ouroboros::Config cfg = consensus->getConfig();
    cfg.timeOffset = 60;  // local 60s behind beacon
    consensus->init(cfg);
    EXPECT_EQ(consensus->getConfig().timeOffset, 60);
}

TEST_F(OuroborosTest, ReturnsErrorWhenNoStakeholders) {
    Ouroboros emptyConsensus;
    auto result = emptyConsensus.getSlotLeader(0);
    
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 1);
    EXPECT_FALSE(result.error().message.empty());
}

// Test fixture for tests that need stakeholders
class OuroborosWithStakeholdersTest : public OuroborosTest {
protected:
    void SetUp() override {
        OuroborosTest::SetUp();
        consensus->setStakeholders({{1, 1000}, {2, 2000}, {3, 500}});
    }
};

TEST_F(OuroborosWithStakeholdersTest, ProducesConsistentLeaderAcrossEpochs) {
    uint64_t slot1 = 0;
    uint64_t slot2 = 100;  // Different epoch
    
    auto leader1 = consensus->getSlotLeader(slot1);
    auto leader2 = consensus->getSlotLeader(slot2);
    
    ASSERT_TRUE(leader1.isOk());
    ASSERT_TRUE(leader2.isOk());
    
    // Leaders might be different, but should be from our stakeholder set
    EXPECT_THAT(leader1.value(), AnyOf(Eq(1), Eq(2), Eq(3)));
    EXPECT_THAT(leader2.value(), AnyOf(Eq(1), Eq(2), Eq(3)));
}
