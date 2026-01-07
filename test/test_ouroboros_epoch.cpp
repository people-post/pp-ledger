#include "EpochManager.h"
#include <gtest/gtest.h>

using namespace pp::consensus;

class EpochManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        epochMgr = std::make_unique<EpochManager>(10, 2);
        genesisTime = 1000000000;
        epochMgr->setGenesisTime(genesisTime);
    }

    void TearDown() override {
        epochMgr.reset();
    }

    std::unique_ptr<EpochManager> epochMgr;
    int64_t genesisTime;
};

TEST_F(EpochManagerTest, CreatesWithCorrectConfiguration) {
    EXPECT_EQ(epochMgr->getSlotsPerEpoch(), 10);
    EXPECT_EQ(epochMgr->getSlotDuration(), 2);
}

TEST_F(EpochManagerTest, SetsAndGetsGenesisTime) {
    EXPECT_EQ(epochMgr->getGenesisTime(), genesisTime);
}

TEST_F(EpochManagerTest, CalculatesSlotTimes) {
    int64_t slot0Start = epochMgr->getSlotStartTime(0);
    int64_t slot0End = epochMgr->getSlotEndTime(0);
    
    EXPECT_EQ(slot0Start, genesisTime);
    EXPECT_EQ(slot0End, genesisTime + 2);
    EXPECT_EQ(slot0End - slot0Start, 2);
}

TEST_F(EpochManagerTest, CalculatesSlotStartTimeForAnySlot) {
    int64_t slot5Start = epochMgr->getSlotStartTime(5);
    EXPECT_EQ(slot5Start, genesisTime + 10);
    
    int64_t slot100Start = epochMgr->getSlotStartTime(100);
    EXPECT_EQ(slot100Start, genesisTime + 200);
}

TEST_F(EpochManagerTest, ConvertsSlotToEpoch) {
    EXPECT_EQ(epochMgr->getEpochFromSlot(0), 0);
    EXPECT_EQ(epochMgr->getEpochFromSlot(9), 0);
    EXPECT_EQ(epochMgr->getEpochFromSlot(10), 1);
    EXPECT_EQ(epochMgr->getEpochFromSlot(25), 2);
    EXPECT_EQ(epochMgr->getEpochFromSlot(99), 9);
}

TEST_F(EpochManagerTest, CalculatesSlotInEpoch) {
    EXPECT_EQ(epochMgr->getSlotInEpoch(0), 0);
    EXPECT_EQ(epochMgr->getSlotInEpoch(9), 9);
    EXPECT_EQ(epochMgr->getSlotInEpoch(10), 0);
    EXPECT_EQ(epochMgr->getSlotInEpoch(25), 5);
    EXPECT_EQ(epochMgr->getSlotInEpoch(99), 9);
}

TEST_F(EpochManagerTest, InitializesEpoch) {
    epochMgr->initializeEpoch(0, "nonce_epoch_0");
    
    EXPECT_TRUE(epochMgr->isEpochInitialized(0));
    
    EpochManager::EpochInfo info = epochMgr->getEpochInfo(0);
    EXPECT_EQ(info.number, 0);
    EXPECT_EQ(info.startSlot, 0);
    EXPECT_EQ(info.endSlot, 9);
    EXPECT_EQ(info.nonce, "nonce_epoch_0");
    EXPECT_EQ(info.startTime, genesisTime);
}

TEST_F(EpochManagerTest, ReturnsEpochInfoForUninitializedEpoch) {
    // Should still return calculated info even if not explicitly initialized
    EpochManager::EpochInfo info = epochMgr->getEpochInfo(5);
    EXPECT_EQ(info.number, 5);
    EXPECT_EQ(info.startSlot, 50);
    EXPECT_EQ(info.endSlot, 59);
}

TEST_F(EpochManagerTest, ManagesSlotLeaders) {
    epochMgr->initializeEpoch(0, "nonce_epoch_0");
    
    epochMgr->setSlotLeader(0, 0, "alice");
    epochMgr->setSlotLeader(0, 1, "bob");
    epochMgr->setSlotLeader(0, 2, "charlie");
    
    EXPECT_EQ(epochMgr->getSlotLeader(0, 0), "alice");
    EXPECT_EQ(epochMgr->getSlotLeader(0, 1), "bob");
    EXPECT_EQ(epochMgr->getSlotLeader(0, 2), "charlie");
}

TEST_F(EpochManagerTest, ReturnsEmptyStringForUnsetSlotLeader) {
    epochMgr->initializeEpoch(0, "nonce_epoch_0");
    EXPECT_EQ(epochMgr->getSlotLeader(0, 5), "");
}

TEST_F(EpochManagerTest, FinalizesEpoch) {
    epochMgr->initializeEpoch(0, "nonce_epoch_0");
    std::vector<std::string> blockHashes = {"hash1", "hash2", "hash3"};
    
    // Should not throw
    EXPECT_NO_THROW(epochMgr->finalizeEpoch(0, blockHashes));
}

TEST_F(EpochManagerTest, HandlesMultipleEpochs) {
    epochMgr->initializeEpoch(0, "nonce_epoch_0");
    epochMgr->initializeEpoch(1, "nonce_epoch_1");
    epochMgr->initializeEpoch(2, "nonce_epoch_2");
    
    EpochManager::EpochInfo epoch0 = epochMgr->getEpochInfo(0);
    EpochManager::EpochInfo epoch1 = epochMgr->getEpochInfo(1);
    EpochManager::EpochInfo epoch2 = epochMgr->getEpochInfo(2);
    
    EXPECT_EQ(epoch0.startSlot, 0);
    EXPECT_EQ(epoch0.endSlot, 9);
    
    EXPECT_EQ(epoch1.startSlot, 10);
    EXPECT_EQ(epoch1.endSlot, 19);
    
    EXPECT_EQ(epoch2.startSlot, 20);
    EXPECT_EQ(epoch2.endSlot, 29);
}

TEST_F(EpochManagerTest, UpdatesSlotsPerEpoch) {
    epochMgr->setSlotsPerEpoch(20);
    EXPECT_EQ(epochMgr->getSlotsPerEpoch(), 20);
    
    // Verify the change affects calculations
    EXPECT_EQ(epochMgr->getEpochFromSlot(19), 0);
    EXPECT_EQ(epochMgr->getEpochFromSlot(20), 1);
}

TEST_F(EpochManagerTest, UpdatesSlotDuration) {
    epochMgr->setSlotDuration(5);
    EXPECT_EQ(epochMgr->getSlotDuration(), 5);
    
    // Verify the change affects time calculations
    int64_t slot1Start = epochMgr->getSlotStartTime(1);
    EXPECT_EQ(slot1Start, genesisTime + 5);
}

// SlotTimer Tests
class SlotTimerTest : public ::testing::Test {
protected:
    void SetUp() override {
        timer = std::make_unique<SlotTimer>(2);
        genesisTime = 1000000000;
    }

    void TearDown() override {
        timer.reset();
    }

    std::unique_ptr<SlotTimer> timer;
    int64_t genesisTime;
};

TEST_F(SlotTimerTest, CreatesWithCorrectDuration) {
    EXPECT_EQ(timer->getSlotDuration(), 2);
}

TEST_F(SlotTimerTest, GetsCurrentTime) {
    int64_t currentTime = timer->getCurrentTime();
    EXPECT_GT(currentTime, 0);
}

TEST_F(SlotTimerTest, CalculatesCurrentSlot) {
    uint64_t slot = timer->getCurrentSlot(genesisTime);
    // Should be some reasonable value
    EXPECT_GE(slot, 0);
}

TEST_F(SlotTimerTest, CalculatesSlotStartAndEndTime) {
    uint64_t testSlot = 100;
    int64_t slotStart = timer->getSlotStartTime(testSlot, genesisTime);
    int64_t slotEnd = timer->getSlotEndTime(testSlot, genesisTime);
    
    EXPECT_EQ(slotStart, genesisTime + 200);
    EXPECT_EQ(slotEnd, genesisTime + 202);
    EXPECT_EQ(slotEnd - slotStart, 2);
}

TEST_F(SlotTimerTest, ValidatesTimeInSlot) {
    uint64_t testSlot = 100;
    int64_t slotStart = timer->getSlotStartTime(testSlot, genesisTime);
    int64_t slotEnd = timer->getSlotEndTime(testSlot, genesisTime);
    
    // Time at slot start should be in slot
    EXPECT_TRUE(timer->isTimeInSlot(slotStart, testSlot, genesisTime));
    
    // Time in middle of slot should be in slot
    EXPECT_TRUE(timer->isTimeInSlot(slotStart + 1, testSlot, genesisTime));
    
    // Time at slot end should NOT be in slot (exclusive)
    EXPECT_FALSE(timer->isTimeInSlot(slotEnd, testSlot, genesisTime));
    
    // Time before slot should not be in slot
    EXPECT_FALSE(timer->isTimeInSlot(slotStart - 1, testSlot, genesisTime));
}

TEST_F(SlotTimerTest, CalculatesTimeUntilSlot) {
    int64_t currentTime = timer->getCurrentTime();
    uint64_t currentSlot = timer->getCurrentSlot(genesisTime);
    uint64_t futureSlot = currentSlot + 10;
    
    int64_t timeUntil = timer->getTimeUntilSlot(futureSlot, genesisTime);
    
    // Should be positive for future slot
    EXPECT_GT(timeUntil, 0);
}

TEST_F(SlotTimerTest, UpdatesSlotDuration) {
    timer->setSlotDuration(5);
    EXPECT_EQ(timer->getSlotDuration(), 5);
    
    // Verify the change affects calculations
    int64_t slot1Start = timer->getSlotStartTime(1, genesisTime);
    EXPECT_EQ(slot1Start, genesisTime + 5);
}
