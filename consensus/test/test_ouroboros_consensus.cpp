#include "Ouroboros.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace pp::consensus;
using ::testing::Ge;
using ::testing::Le;
using ::testing::AnyOf;
using ::testing::Eq;

class OuroborosTest : public ::testing::Test {
protected:
    void SetUp() override {
        consensus = std::make_unique<Ouroboros>();
        consensus->setSlotDuration(5);
        consensus->setSlotsPerEpoch(10);
    }

    void TearDown() override {
        consensus.reset();
    }

    std::unique_ptr<Ouroboros> consensus;
};

TEST_F(OuroborosTest, CreatesWithCorrectConfiguration) {
    EXPECT_EQ(consensus->getSlotDuration(), 5);
    EXPECT_EQ(consensus->getSlotsPerEpoch(), 10);
}

TEST_F(OuroborosTest, RegistersStakeholders) {
    consensus->registerStakeholder(1, 1000);
    consensus->registerStakeholder(2, 2000);
    consensus->registerStakeholder(3, 500);
    consensus->registerStakeholder(4, 1500);
    
    EXPECT_EQ(consensus->getStakeholderCount(), 4);
    EXPECT_EQ(consensus->getTotalStake(), 5000);
}

TEST_F(OuroborosTest, RejectsZeroStake) {
    consensus->registerStakeholder(1, 1000);
    size_t countBefore = consensus->getStakeholderCount();
    
    consensus->registerStakeholder(100, 0);
    
    EXPECT_EQ(consensus->getStakeholderCount(), countBefore);
}

TEST_F(OuroborosTest, CalculatesSlotAndEpoch) {
    uint64_t currentSlot = consensus->getCurrentSlot();
    uint64_t currentEpoch = consensus->getCurrentEpoch();
    uint64_t slotInEpoch = consensus->getSlotInEpoch(currentSlot);
    
    EXPECT_LT(slotInEpoch, 10);
    EXPECT_EQ(currentEpoch, currentSlot / 10);
}

TEST_F(OuroborosTest, SelectsSlotLeadersDeterministically) {
    consensus->registerStakeholder(1, 1000);
    consensus->registerStakeholder(2, 2000);
    consensus->registerStakeholder(3, 500);
    consensus->registerStakeholder(4, 1500);
    
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
    consensus->registerStakeholder(1, 1000);
    consensus->registerStakeholder(2, 2000);
    
    uint64_t currentSlot = consensus->getCurrentSlot();
    auto leaderResult = consensus->getSlotLeader(currentSlot);
    
    ASSERT_TRUE(leaderResult.isOk());
    uint64_t currentLeader = leaderResult.value();
    
    EXPECT_TRUE(consensus->isSlotLeader(currentSlot, currentLeader));
    
    // Check non-leader
    uint64_t nonLeader = (currentLeader == 1) ? 2 : 1;
    EXPECT_FALSE(consensus->isSlotLeader(currentSlot, nonLeader));
}

TEST_F(OuroborosTest, UpdatesStake) {
    consensus->registerStakeholder(1, 1000);
    consensus->registerStakeholder(2, 2000);
    
    uint64_t oldTotal = consensus->getTotalStake();
    consensus->updateStake(1, 1500);
    
    EXPECT_EQ(consensus->getTotalStake(), oldTotal + 500);
}

TEST_F(OuroborosTest, HandlesStakeUpdateForUnknownStakeholder) {
    consensus->registerStakeholder(1, 1000);
    uint64_t oldTotal = consensus->getTotalStake();
    
    consensus->updateStake(100, 2000);
    
    // Total should not change
    EXPECT_EQ(consensus->getTotalStake(), oldTotal);
}

TEST_F(OuroborosTest, RemovesStakeholder) {
    consensus->registerStakeholder(1, 1000);
    consensus->registerStakeholder(2, 2000);
    consensus->registerStakeholder(3, 500);
    
    bool removed = consensus->removeStakeholder(3);
    
    EXPECT_TRUE(removed);
    EXPECT_EQ(consensus->getStakeholderCount(), 2);
}

TEST_F(OuroborosTest, FailsToRemoveNonExistentStakeholder) {
    consensus->registerStakeholder(1, 1000);
    
    bool removed = consensus->removeStakeholder(100);
    
    EXPECT_FALSE(removed);
    EXPECT_EQ(consensus->getStakeholderCount(), 1);
}

TEST_F(OuroborosTest, ReturnsAllStakeholders) {
    consensus->registerStakeholder(1, 1500);
    consensus->registerStakeholder(2, 2000);
    consensus->registerStakeholder(3, 1500);
    
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
    consensus->setSlotDuration(10);
    EXPECT_EQ(consensus->getSlotDuration(), 10);
}

TEST_F(OuroborosTest, UpdatesSlotsPerEpoch) {
    consensus->setSlotsPerEpoch(20);
    EXPECT_EQ(consensus->getSlotsPerEpoch(), 20);
}

TEST_F(OuroborosTest, SetsGenesisTime) {
    int64_t genesisTime = 1234567890;
    consensus->setGenesisTime(genesisTime);
    EXPECT_EQ(consensus->getGenesisTime(), genesisTime);
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
        consensus->registerStakeholder(1, 1000);
        consensus->registerStakeholder(2, 2000);
        consensus->registerStakeholder(3, 500);
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
    EXPECT_THAT(leader1.value(), AnyOf(1, 2, 3));
    EXPECT_THAT(leader2.value(), AnyOf(1, 2, 3));
}
