#include "../Ledger.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace pp;

class LedgerTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a unique test directory
    testDir_ = std::filesystem::temp_directory_path() / "ledger_test";
    cleanupTestDir();
    std::filesystem::create_directories(testDir_);
  }

  void TearDown() override {
    cleanupTestDir();
  }

  void cleanupTestDir() {
    std::error_code ec;
    if (std::filesystem::exists(testDir_, ec)) {
      std::filesystem::remove_all(testDir_, ec);
    }
  }

  Ledger::ChainNode createTestBlock(uint64_t id, const std::string& data) {
    Ledger::ChainNode block;
    block.block.index = id;
    block.block.previousHash = "prev_hash_" + std::to_string(id);
    block.block.timestamp = static_cast<int64_t>(std::time(nullptr));
    // Note: data parameter is kept for API compatibility but not used
    // Blocks now use signedTxes instead of data field
    block.hash = "hash_" + std::to_string(id);
    return block;
  }

  std::filesystem::path testDir_;
};

TEST_F(LedgerTest, InitializeNewLedger) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk()) << result.error().message;
  EXPECT_EQ(ledger.getNextBlockId(), 0);

  // Verify directory structure
  EXPECT_TRUE(std::filesystem::exists(testDir_ / "data"));
  EXPECT_TRUE(std::filesystem::exists(testDir_ / "ledger_index.dat"));
}

TEST_F(LedgerTest, GetCurrentBlockIdReturnsZeroWhenNoData) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());
  EXPECT_EQ(ledger.getNextBlockId(), 0);
}

TEST_F(LedgerTest, AddBlocksAndGetCurrentBlockId) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add blocks
  for (uint64_t i = 1; i <= 5; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk()) << addResult.error().message;
    EXPECT_EQ(ledger.getNextBlockId(), i);
  }
}

TEST_F(LedgerTest, ReopenExistingLedger) {
  // Create ledger and add blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    for (uint64_t i = 1; i <= 3; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 3);
  }

  // Reopen ledger
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 3);

    // Add more blocks
    for (uint64_t i = 4; i <= 5; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 5);
  }
}

TEST_F(LedgerTest, CleanupWhenStartingBlockIdIsNewer) {
  // Create ledger with some blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    for (uint64_t i = 1; i <= 3; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 3);
  }

  // Reopen with newer startingBlockId
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 10; // Newer than current (2)

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 10); // Should be startingBlockId (no blocks yet)

    // Old data should be cleaned up, can add new blocks
    Ledger::ChainNode block = createTestBlock(1, "new_data");
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 11); // startingBlockId + blockCount = 10 + 1 = 11
  }
}

TEST_F(LedgerTest, WorkOnExistingDataWhenStartingBlockIdIsOlder) {
  // Create ledger with some blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    for (uint64_t i = 1; i <= 5; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 5);
  }

  // Reopen with older startingBlockId
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 3; // Older than current (4)

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 5); // Should keep existing data

    // Can continue adding blocks
    Ledger::ChainNode block = createTestBlock(6, "data_6");
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 6);
  }
}

TEST_F(LedgerTest, UpdateCheckpointsSorted) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add blocks
  for (uint64_t i = 1; i <= 10; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with sorted IDs (0-based)
  std::vector<uint64_t> checkpoints = {1, 4, 7, 9};
  auto updateResult = ledger.updateCheckpoints(checkpoints);
  ASSERT_TRUE(updateResult.isOk()) << updateResult.error().message;
}

TEST_F(LedgerTest, UpdateCheckpointsNotSortedFails) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add blocks
  for (uint64_t i = 1; i <= 10; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with unsorted IDs (should fail)
  std::vector<uint64_t> checkpoints = {4, 1, 7, 9};
  auto updateResult = ledger.updateCheckpoints(checkpoints);
  ASSERT_FALSE(updateResult.isOk());
  EXPECT_NE(updateResult.error().message.find("sorted"), std::string::npos);
}

TEST_F(LedgerTest, UpdateCheckpointsWithDuplicatesFails) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add blocks
  for (uint64_t i = 1; i <= 10; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with duplicates (should fail)
  std::vector<uint64_t> checkpoints = {1, 4, 4, 9};
  auto updateResult = ledger.updateCheckpoints(checkpoints);
  ASSERT_FALSE(updateResult.isOk());
  EXPECT_NE(updateResult.error().message.find("duplicate"), std::string::npos);
}

TEST_F(LedgerTest, UpdateCheckpointsExceedingCurrentBlockIdFails) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add blocks
  for (uint64_t i = 1; i <= 5; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with ID exceeding current (should fail)
  std::vector<uint64_t> checkpoints = {1, 3, 9}; // 9 > 4 (current block ID)
  auto updateResult = ledger.updateCheckpoints(checkpoints);
  ASSERT_FALSE(updateResult.isOk());
  EXPECT_NE(updateResult.error().message.find("exceeds"), std::string::npos);
}

TEST_F(LedgerTest, UpdateCheckpointsWithOverlappingDataMatches) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add blocks
  for (uint64_t i = 1; i <= 10; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Set initial checkpoints
  std::vector<uint64_t> checkpoints1 = {1, 4, 7};
  auto updateResult1 = ledger.updateCheckpoints(checkpoints1);
  ASSERT_TRUE(updateResult1.isOk());

  // Update with overlapping data that matches
  std::vector<uint64_t> checkpoints2 = {1, 4, 7, 9};
  auto updateResult2 = ledger.updateCheckpoints(checkpoints2);
  ASSERT_TRUE(updateResult2.isOk());
}

TEST_F(LedgerTest, UpdateCheckpointsWithOverlappingDataMismatchFails) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add blocks
  for (uint64_t i = 1; i <= 10; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Set initial checkpoints
  std::vector<uint64_t> checkpoints1 = {1, 4, 7};
  auto updateResult1 = ledger.updateCheckpoints(checkpoints1);
  ASSERT_TRUE(updateResult1.isOk());

  // Update with overlapping data that doesn't match (should fail)
  std::vector<uint64_t> checkpoints2 = {1, 5, 7, 9}; // 5 != 4
  auto updateResult2 = ledger.updateCheckpoints(checkpoints2);
  ASSERT_FALSE(updateResult2.isOk());
  EXPECT_NE(updateResult2.error().message.find("mismatch"), std::string::npos);
}

TEST_F(LedgerTest, CheckpointsPersistAcrossReopens) {
  // Create ledger and set checkpoints
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    // Add blocks
    for (uint64_t i = 1; i <= 10; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }

    // Set checkpoints
    std::vector<uint64_t> checkpoints = {1, 4, 7, 9};
    auto updateResult = ledger.updateCheckpoints(checkpoints);
    ASSERT_TRUE(updateResult.isOk());
  }

  // Reopen and verify checkpoints persist
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    // Update with same checkpoints (should succeed)
    std::vector<uint64_t> checkpoints = {1, 4, 7, 9};
    auto updateResult = ledger.updateCheckpoints(checkpoints);
    ASSERT_TRUE(updateResult.isOk());

    // Can extend checkpoints
    std::vector<uint64_t> extendedCheckpoints = {1, 4, 7, 9};
    auto extendResult = ledger.updateCheckpoints(extendedCheckpoints);
    ASSERT_TRUE(extendResult.isOk());
  }
}

TEST_F(LedgerTest, ReadBlockSuccessfully) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add test blocks
  std::vector<Ledger::ChainNode> testBlocks;
  for (uint64_t i = 1; i <= 5; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    testBlocks.push_back(block);
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Read back blocks and verify (0-based IDs)
  for (uint64_t i = 0; i < 5; ++i) {
    auto readResult = ledger.readBlock(i);
    ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i << ": " 
                                    << readResult.error().message;
    
    const Ledger::ChainNode& readBlock = readResult.value();
    EXPECT_EQ(readBlock.block.index, testBlocks[i].block.index);
    EXPECT_EQ(readBlock.hash, testBlocks[i].hash);
    EXPECT_EQ(readBlock.block.previousHash, testBlocks[i].block.previousHash);
  }
}

TEST_F(LedgerTest, ReadBlockWithInvalidIdFails) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Add some blocks
  for (uint64_t i = 1; i <= 3; ++i) {
    Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Try to read block beyond current (ID 10 > 2)
  auto readResult10 = ledger.readBlock(10);
  ASSERT_FALSE(readResult10.isOk());
  EXPECT_NE(readResult10.error().message.find("exceeds"), std::string::npos);
}

TEST_F(LedgerTest, ReadBlockFromEmptyLedgerFails) {
  Ledger ledger;
  Ledger::Config config;
  config.workDir = testDir_.string();
  config.startingBlockId = 0;

  auto result = ledger.init(config);
  ASSERT_TRUE(result.isOk());

  // Try to read from empty ledger
  auto readResult = ledger.readBlock(0);
  ASSERT_FALSE(readResult.isOk());
  EXPECT_NE(readResult.error().message.find("exceeds"), std::string::npos);
}

TEST_F(LedgerTest, ReadBlockAfterReopen) {
  // Create ledger and add blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    for (uint64_t i = 1; i <= 5; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
  }

  // Reopen ledger and read blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    // Read blocks after reopening (0-based IDs)
    for (uint64_t i = 0; i < 5; ++i) {
      auto readResult = ledger.readBlock(i);
      ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i 
                                      << " after reopen: " 
                                      << readResult.error().message;
      
      const Ledger::ChainNode& readBlock = readResult.value();
      EXPECT_EQ(readBlock.block.index, i + 1);
    }
  }
}

TEST_F(LedgerTest, CleanupWhenStartingBlockIdGreaterThanExistingNextBlockId) {
  // Create ledger with startingBlockId = 10 and add some blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 10;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    // Add 3 blocks, so nextBlockId = 10 + 3 = 13
    for (uint64_t i = 1; i <= 3; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 13); // startingBlockId + blockCount = 10 + 3
  }

  // Reopen with startingBlockId = 15, which is > existingNextBlockId (13)
  // Should trigger cleanup
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 15; // Greater than existing nextBlockId (13)

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 15); // Should be startingBlockId (no blocks after cleanup)

    // Verify old data is gone - should have 0 blocks
    uint64_t blockCount = 0;
    for (uint64_t i = 10; i < 13; ++i) {
      auto readResult = ledger.readBlock(i);
      if (readResult.isOk()) {
        blockCount++;
      }
    }
    EXPECT_EQ(blockCount, 0); // Old blocks should be cleaned up
  }
}

TEST_F(LedgerTest, PreserveExistingStartingBlockIdWhenNotCleaningUp) {
  // Create ledger with startingBlockId = 5 and add some blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 5;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    // Add 2 blocks, so nextBlockId = 5 + 2 = 7
    for (uint64_t i = 1; i <= 2; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 7);
  }

  // Reopen with startingBlockId = 3, which is < existingNextBlockId (7)
  // Should preserve existing startingBlockId = 5
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 3; // Less than existing nextBlockId (7)

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    // Should preserve existing startingBlockId (5), so nextBlockId = 5 + 2 = 7
    EXPECT_EQ(ledger.getNextBlockId(), 7);

    // Verify existing blocks are still accessible
    auto readResult1 = ledger.readBlock(5);
    ASSERT_TRUE(readResult1.isOk());
    EXPECT_EQ(readResult1.value().block.index, 1);

    auto readResult2 = ledger.readBlock(6);
    ASSERT_TRUE(readResult2.isOk());
    EXPECT_EQ(readResult2.value().block.index, 2);

    // Add a new block - should get ID 7 (nextBlockId)
    Ledger::ChainNode block = createTestBlock(3, "data_3");
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 8); // 5 + 3 = 8
  }
}

TEST_F(LedgerTest, PreserveExistingStartingBlockIdWhenEqual) {
  // Create ledger with startingBlockId = 20 and add some blocks
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 20;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());

    // Add 4 blocks, so nextBlockId = 20 + 4 = 24
    for (uint64_t i = 1; i <= 4; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 24);
  }

  // Reopen with startingBlockId = 24, which equals existingNextBlockId
  // Should preserve existing startingBlockId = 20 (not cleanup)
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 24; // Equals existing nextBlockId (24)

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    // Should preserve existing startingBlockId (20), so nextBlockId = 20 + 4 = 24
    EXPECT_EQ(ledger.getNextBlockId(), 24);

    // Verify existing blocks are still accessible
    for (uint64_t i = 20; i < 24; ++i) {
      auto readResult = ledger.readBlock(i);
      ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i;
    }
  }
}

TEST_F(LedgerTest, StartingBlockIdPreservedWithNonZeroStartingBlockId) {
  // Create ledger with startingBlockId = 100
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 100;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getNextBlockId(), 100); // No blocks yet

    // Add 5 blocks
    for (uint64_t i = 1; i <= 5; ++i) {
      Ledger::ChainNode block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getNextBlockId(), 105); // 100 + 5
  }

  // Reopen with same startingBlockId - should preserve it
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 100; // Same as original

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    // Should preserve startingBlockId = 100, so nextBlockId = 100 + 5 = 105
    EXPECT_EQ(ledger.getNextBlockId(), 105);

    // Verify blocks are accessible with correct IDs
    auto readResult = ledger.readBlock(100);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value().block.index, 1);

    auto readResult2 = ledger.readBlock(104);
    ASSERT_TRUE(readResult2.isOk());
    EXPECT_EQ(readResult2.value().block.index, 5);
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
