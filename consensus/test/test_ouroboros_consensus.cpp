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
    consensus->registerStakeholder("alice", 1000);
    consensus->registerStakeholder("bob", 2000);
    consensus->registerStakeholder("charlie", 500);
    consensus->registerStakeholder("dave", 1500);
    
    EXPECT_EQ(consensus->getStakeholderCount(), 4);
    EXPECT_EQ(consensus->getTotalStake(), 5000);
}

TEST_F(OuroborosTest, RejectsZeroStake) {
    consensus->registerStakeholder("alice", 1000);
    size_t countBefore = consensus->getStakeholderCount();
    
    consensus->registerStakeholder("zero_stake", 0);
    
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
    consensus->registerStakeholder("alice", 1000);
    consensus->registerStakeholder("bob", 2000);
    consensus->registerStakeholder("charlie", 500);
    consensus->registerStakeholder("dave", 1500);
    
    uint64_t currentSlot = consensus->getCurrentSlot();
    
    // Select leaders for 5 consecutive slots
    std::vector<std::string> leaders;
    for (uint64_t i = 0; i < 5; i++) {
        uint64_t slot = currentSlot + i;
        auto leaderResult = consensus->getSlotLeader(slot);
        
        ASSERT_TRUE(leaderResult.isOk());
        std::string leader = leaderResult.value();
        leaders.push_back(leader);
        
        // Verify leader is one of our stakeholders
        EXPECT_THAT(leader, AnyOf(Eq("alice"), Eq("bob"), Eq("charlie"), Eq("dave")));
    }
    
    // Verify determinism: same slot always returns same leader
    for (uint64_t i = 0; i < 5; i++) {
        uint64_t slot = currentSlot + i;
        auto leaderResult = consensus->getSlotLeader(slot);
        ASSERT_TRUE(leaderResult.isOk());
        EXPECT_EQ(leaderResult.value(), leaders[i]);
    }
}

TEST_F(OuroborosTest, VerifiesSlotLeadership) {
    consensus->registerStakeholder("alice", 1000);
    consensus->registerStakeholder("bob", 2000);
    
    uint64_t currentSlot = consensus->getCurrentSlot();
    auto leaderResult = consensus->getSlotLeader(currentSlot);
    
    ASSERT_TRUE(leaderResult.isOk());
    std::string currentLeader = leaderResult.value();
    
    EXPECT_TRUE(consensus->isSlotLeader(currentSlot, currentLeader));
    
    // Check non-leader
    std::string nonLeader = (currentLeader == "alice") ? "bob" : "alice";
    EXPECT_FALSE(consensus->isSlotLeader(currentSlot, nonLeader));
}

TEST_F(OuroborosTest, UpdatesStake) {
    consensus->registerStakeholder("alice", 1000);
    consensus->registerStakeholder("bob", 2000);
    
    uint64_t oldTotal = consensus->getTotalStake();
    consensus->updateStake("alice", 1500);
    
    EXPECT_EQ(consensus->getTotalStake(), oldTotal + 500);
}

TEST_F(OuroborosTest, HandlesStakeUpdateForUnknownStakeholder) {
    consensus->registerStakeholder("alice", 1000);
    uint64_t oldTotal = consensus->getTotalStake();
    
    consensus->updateStake("unknown", 2000);
    
    // Total should not change
    EXPECT_EQ(consensus->getTotalStake(), oldTotal);
}

TEST_F(OuroborosTest, RemovesStakeholder) {
    consensus->registerStakeholder("alice", 1000);
    consensus->registerStakeholder("bob", 2000);
    consensus->registerStakeholder("charlie", 500);
    
    bool removed = consensus->removeStakeholder("charlie");
    
    EXPECT_TRUE(removed);
    EXPECT_EQ(consensus->getStakeholderCount(), 2);
}

TEST_F(OuroborosTest, FailsToRemoveNonExistentStakeholder) {
    consensus->registerStakeholder("alice", 1000);
    
    bool removed = consensus->removeStakeholder("nonexistent");
    
    EXPECT_FALSE(removed);
    EXPECT_EQ(consensus->getStakeholderCount(), 1);
}

TEST_F(OuroborosTest, ReturnsAllStakeholders) {
    consensus->registerStakeholder("alice", 1500);
    consensus->registerStakeholder("bob", 2000);
    consensus->registerStakeholder("dave", 1500);
    
    auto stakeholders = consensus->getStakeholders();
    
    EXPECT_EQ(stakeholders.size(), 3);
    
    // Verify all stakeholders are present
    std::set<std::string> ids;
    for (const auto& sh : stakeholders) {
        ids.insert(sh.id);
    }
    
    EXPECT_TRUE(ids.count("alice"));
    EXPECT_TRUE(ids.count("bob"));
    EXPECT_TRUE(ids.count("dave"));
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
        consensus->registerStakeholder("alice", 1000);
        consensus->registerStakeholder("bob", 2000);
        consensus->registerStakeholder("charlie", 500);
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
    EXPECT_THAT(leader1.value(), AnyOf(Eq("alice"), Eq("bob"), Eq("charlie")));
    EXPECT_THAT(leader2.value(), AnyOf(Eq("alice"), Eq("bob"), Eq("charlie")));
}
