#include "../Ledger.h"
#include "../Block.h"
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

  Block createTestBlock(uint64_t id, const std::string& data) {
    Block block;
    block.setIndex(id);
    block.setPreviousHash("prev_hash_" + std::to_string(id));
    block.setTimestamp(static_cast<int64_t>(std::time(nullptr)));
    block.setData(data);
    block.setHash("hash_" + std::to_string(id));
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
  EXPECT_EQ(ledger.getCurrentBlockId(), 0);

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
  EXPECT_EQ(ledger.getCurrentBlockId(), 0);
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk()) << addResult.error().message;
    EXPECT_EQ(ledger.getCurrentBlockId(), i);
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
      Block block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getCurrentBlockId(), 3);
  }

  // Reopen ledger
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 0;

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getCurrentBlockId(), 3);

    // Add more blocks
    for (uint64_t i = 4; i <= 5; ++i) {
      Block block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getCurrentBlockId(), 5);
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
      Block block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getCurrentBlockId(), 3);
  }

  // Reopen with newer startingBlockId
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 10; // Newer than current (3)

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getCurrentBlockId(), 0); // Should be reset

    // Old data should be cleaned up, can add new blocks
    Block block = createTestBlock(1, "new_data");
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
    EXPECT_EQ(ledger.getCurrentBlockId(), 1);
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
      Block block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }
    EXPECT_EQ(ledger.getCurrentBlockId(), 5);
  }

  // Reopen with older startingBlockId
  {
    Ledger ledger;
    Ledger::Config config;
    config.workDir = testDir_.string();
    config.startingBlockId = 3; // Older than current (5)

    auto result = ledger.init(config);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(ledger.getCurrentBlockId(), 5); // Should keep existing data

    // Can continue adding blocks
    Block block = createTestBlock(6, "data_6");
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
    EXPECT_EQ(ledger.getCurrentBlockId(), 6);
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with sorted IDs
  std::vector<uint64_t> checkpoints = {2, 5, 8, 10};
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with unsorted IDs (should fail)
  std::vector<uint64_t> checkpoints = {5, 2, 8, 10};
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with duplicates (should fail)
  std::vector<uint64_t> checkpoints = {2, 5, 5, 10};
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Update checkpoints with ID exceeding current (should fail)
  std::vector<uint64_t> checkpoints = {2, 4, 10}; // 10 > 5
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Set initial checkpoints
  std::vector<uint64_t> checkpoints1 = {2, 5, 8};
  auto updateResult1 = ledger.updateCheckpoints(checkpoints1);
  ASSERT_TRUE(updateResult1.isOk());

  // Update with overlapping data that matches
  std::vector<uint64_t> checkpoints2 = {2, 5, 8, 10};
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Set initial checkpoints
  std::vector<uint64_t> checkpoints1 = {2, 5, 8};
  auto updateResult1 = ledger.updateCheckpoints(checkpoints1);
  ASSERT_TRUE(updateResult1.isOk());

  // Update with overlapping data that doesn't match (should fail)
  std::vector<uint64_t> checkpoints2 = {2, 6, 8, 10}; // 6 != 5
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
      Block block = createTestBlock(i, "data_" + std::to_string(i));
      auto addResult = ledger.addBlock(block);
      ASSERT_TRUE(addResult.isOk());
    }

    // Set checkpoints
    std::vector<uint64_t> checkpoints = {2, 5, 8, 10};
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
    std::vector<uint64_t> checkpoints = {2, 5, 8, 10};
    auto updateResult = ledger.updateCheckpoints(checkpoints);
    ASSERT_TRUE(updateResult.isOk());

    // Can extend checkpoints
    std::vector<uint64_t> extendedCheckpoints = {2, 5, 8, 10};
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
  std::vector<Block> testBlocks;
  for (uint64_t i = 1; i <= 5; ++i) {
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    testBlocks.push_back(block);
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Read back blocks and verify
  for (uint64_t i = 1; i <= 5; ++i) {
    auto readResult = ledger.readBlock(i);
    ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i << ": " 
                                    << readResult.error().message;
    
    const Block& readBlock = readResult.value();
    EXPECT_EQ(readBlock.getIndex(), testBlocks[i-1].getIndex());
    EXPECT_EQ(readBlock.getData(), testBlocks[i-1].getData());
    EXPECT_EQ(readBlock.getHash(), testBlocks[i-1].getHash());
    EXPECT_EQ(readBlock.getPreviousHash(), testBlocks[i-1].getPreviousHash());
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
    Block block = createTestBlock(i, "data_" + std::to_string(i));
    auto addResult = ledger.addBlock(block);
    ASSERT_TRUE(addResult.isOk());
  }

  // Try to read block with ID 0 (invalid)
  auto readResult0 = ledger.readBlock(0);
  ASSERT_FALSE(readResult0.isOk());
  EXPECT_NE(readResult0.error().message.find("greater than 0"), std::string::npos);

  // Try to read block beyond current (ID 10 > 3)
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
  auto readResult = ledger.readBlock(1);
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
      Block block = createTestBlock(i, "data_" + std::to_string(i));
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

    // Read blocks after reopening
    for (uint64_t i = 1; i <= 5; ++i) {
      auto readResult = ledger.readBlock(i);
      ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i 
                                      << " after reopen: " 
                                      << readResult.error().message;
      
      const Block& readBlock = readResult.value();
      EXPECT_EQ(readBlock.getIndex(), i);
      EXPECT_EQ(readBlock.getData(), "data_" + std::to_string(i));
    }
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
