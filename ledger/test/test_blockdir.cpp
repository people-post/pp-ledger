#include "BlockDir.h"
#include "Block.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

class BlockDirTest : public ::testing::Test {
protected:
    std::string testDir;
    pp::BlockDir blockDir;
    pp::BlockDir::Config config;
    
    void SetUp() override {
        testDir = "/tmp/pp-ledger-blockdir-test";
        
        // Clean up test directory
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
        std::filesystem::create_directories(testDir);
        
        config = pp::BlockDir::Config(testDir, 1024 * 1024); // 1MB max file size
    }
    
    void TearDown() override {
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
    }
    
    // Helper to create a Block object
    std::shared_ptr<pp::Block> createTestBlock(uint64_t index, const std::string& data = "") {
        auto block = std::make_shared<pp::Block>();
        block->setIndex(index);
        block->setData(data.empty() ? "Test data " + std::to_string(index) : data);
        block->setTimestamp(static_cast<int64_t>(index * 1000));
        block->setPreviousHash(index == 0 ? "0" : std::to_string(index - 1));
        block->setNonce(index);
        block->setSlot(index);
        block->setSlotLeader("leader_" + std::to_string(index));
        // Calculate and set the hash after all fields are set
        block->setHash(block->calculateHash());
        return block;
    }
};

// ============================================================================
// Initialization Tests
// ============================================================================

TEST_F(BlockDirTest, InitializesSuccessfully) {
    auto result = blockDir.init(config, false);
    EXPECT_TRUE(result.isOk());
    
    // Directory should exist
    EXPECT_TRUE(std::filesystem::exists(testDir));
}

TEST_F(BlockDirTest, InitializesWithBlockchainManagement) {
    auto result = blockDir.init(config, true);
    EXPECT_TRUE(result.isOk());
    
    // Blockchain should be initialized
    EXPECT_EQ(blockDir.getBlockchainSize(), 0);
    EXPECT_EQ(blockDir.getLatestBlock(), nullptr);
    EXPECT_FALSE(blockDir.isBlockchainValid());
    EXPECT_EQ(blockDir.getLastBlockHash(), "0");
}

TEST_F(BlockDirTest, LoadsExistingIndex) {
    // First initialization and add a block
    blockDir.init(config, true);
    auto block = createTestBlock(0);
    ASSERT_TRUE(blockDir.addBlock(block));
    
    // Reinitialize - should load existing index
    pp::BlockDir blockDir2;
    auto result = blockDir2.init(config, true);
    EXPECT_TRUE(result.isOk());
    
    // Should have the block in blockchain
    EXPECT_EQ(blockDir2.getBlockchainSize(), 1);
    EXPECT_NE(blockDir2.getBlock(0), nullptr);
}

// ============================================================================
// Block Writing Tests
// ============================================================================

TEST_F(BlockDirTest, WritesSingleBlock) {
    blockDir.init(config, true);
    
    auto block = createTestBlock(0);
    bool result = blockDir.addBlock(block);
    
    EXPECT_TRUE(result);
    EXPECT_GT(blockDir.getTotalStorageSize(), 0);
    EXPECT_EQ(blockDir.getBlockchainSize(), 1);
}

TEST_F(BlockDirTest, WritesMultipleBlocks) {
    blockDir.init(config, true);
    
    const size_t numBlocks = 10;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        bool result = blockDir.addBlock(block);
        ASSERT_TRUE(result) << "Failed to add block " << i;
    }
    
    EXPECT_GT(blockDir.getTotalStorageSize(), 0);
    EXPECT_EQ(blockDir.getBlockchainSize(), numBlocks);
}

TEST_F(BlockDirTest, CreatesNewFileWhenMaxSizeReached) {
    // Use small max file size to trigger file rollover (1MB minimum for BlockFile)
    pp::BlockDir::Config smallConfig(testDir, 1024 * 1024); // 1MB max size
    blockDir.init(smallConfig, true);
    
    // Add blocks with large data to trigger file rollover
    // Each block serialized will be larger than just the data, so use ~80KB data per block
    // This should trigger rollover after ~12 blocks (12 * 80KB = ~960KB, plus serialization overhead)
    std::string largeData(80 * 1024, 'X'); // 80KB per block
    
    size_t initialSize = blockDir.getTotalStorageSize();
    const size_t numBlocks = 15; // Enough to trigger rollover
    
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i, largeData);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        ASSERT_TRUE(blockDir.addBlock(block)) << "Failed to add block " << i;
    }
    
    // Should have written multiple blocks
    EXPECT_GT(blockDir.getBlockchainSize(), 0);
    EXPECT_GT(blockDir.getTotalStorageSize(), initialSize);
    
    // With 1MB max file size and ~80KB blocks, we should have created multiple files
    // Total storage should exceed 1MB (indicating multiple files)
    EXPECT_GT(blockDir.getTotalStorageSize(), 1024 * 1024);
}

// ============================================================================
// Index Persistence Tests
// ============================================================================

TEST_F(BlockDirTest, PersistsIndexToDisk) {
    blockDir.init(config, true);
    
    // Add some blocks (which writes to storage and saves index)
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        blockDir.addBlock(block);
    }
    
    // Index file should exist after adding blocks
    std::string indexFile = testDir + "/idx.dat";
    EXPECT_TRUE(std::filesystem::exists(indexFile));
    EXPECT_GT(std::filesystem::file_size(indexFile), 0);
}

TEST_F(BlockDirTest, LoadsIndexFromDisk) {
    blockDir.init(config, true);
    
    // Add blocks
    const size_t numBlocks = 10;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        blockDir.addBlock(block);
    }
    
    // Reinitialize and verify blocks are loaded
    pp::BlockDir blockDir2;
    blockDir2.init(config, true);
    
    // Blocks should be loaded into blockchain
    EXPECT_EQ(blockDir2.getBlockchainSize(), numBlocks);
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = blockDir2.getBlock(i);
        ASSERT_NE(block, nullptr) << "Block " << i << " not found after reload";
        EXPECT_EQ(block->getIndex(), i);
    }
}

// ============================================================================
// File Management Tests
// ============================================================================

TEST_F(BlockDirTest, GetTotalStorageSize) {
    blockDir.init(config, true);
    
    EXPECT_EQ(blockDir.getTotalStorageSize(), 0);
    
    // Add blocks and verify size increases
    auto block = createTestBlock(0);
    blockDir.addBlock(block);
    
    EXPECT_GT(blockDir.getTotalStorageSize(), 0);
}

TEST_F(BlockDirTest, MoveFrontFileToTarget) {
    // Create source and target directories
    std::string sourceDir = testDir + "/source";
    std::string targetDir = testDir + "/target";
    std::filesystem::create_directories(sourceDir);
    std::filesystem::create_directories(targetDir);
    
    pp::BlockDir source;
    pp::BlockDir target;
    
    // Use 1MB minimum (BlockFile requirement)
    pp::BlockDir::Config sourceConfig(sourceDir, 1024 * 1024);
    pp::BlockDir::Config targetConfig(targetDir, 1024 * 1024);
    
    source.init(sourceConfig, true);
    target.init(targetConfig, true);
    
    // Add blocks to source
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = source.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        source.addBlock(block);
    }
    
    // Get source storage size before move
    size_t sourceSizeBefore = source.getTotalStorageSize();
    EXPECT_GT(sourceSizeBefore, 0);
    
    // Move front file to target
    auto result = source.moveFrontFileTo(target);
    EXPECT_TRUE(result.isOk());
    
    // Verify file was moved - target should now have storage
    EXPECT_GT(target.getTotalStorageSize(), 0);
}

// ============================================================================
// Blockchain Management Tests
// ============================================================================

TEST_F(BlockDirTest, AddsBlockToBlockchain) {
    blockDir.init(config, true);
    
    auto block = createTestBlock(0);
    bool result = blockDir.addBlock(block);
    
    EXPECT_TRUE(result);
    EXPECT_EQ(blockDir.getBlockchainSize(), 1);
    EXPECT_NE(blockDir.getLatestBlock(), nullptr);
    EXPECT_EQ(blockDir.getLatestBlock()->getIndex(), 0);
}

TEST_F(BlockDirTest, GetsBlockByIndex) {
    blockDir.init(config, true);
    
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        blockDir.addBlock(block);
    }
    
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = blockDir.getBlock(i);
        ASSERT_NE(block, nullptr) << "Block " << i << " not found";
        EXPECT_EQ(block->getIndex(), i);
    }
    
    // Non-existent block
    EXPECT_EQ(blockDir.getBlock(999), nullptr);
}

TEST_F(BlockDirTest, ValidatesBlockchain) {
    blockDir.init(config, true);
    
    // Empty blockchain should not be valid
    EXPECT_FALSE(blockDir.isBlockchainValid());
    
    // Add valid blocks
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        blockDir.addBlock(block);
    }
    
    EXPECT_TRUE(blockDir.isBlockchainValid());
}

TEST_F(BlockDirTest, GetsLastBlockHash) {
    blockDir.init(config, true);
    
    // Empty blockchain
    EXPECT_EQ(blockDir.getLastBlockHash(), "0");
    
    // Add blocks
    const size_t numBlocks = 3;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        blockDir.addBlock(block);
    }
    
    auto latestBlock = blockDir.getLatestBlock();
    ASSERT_NE(latestBlock, nullptr);
    EXPECT_EQ(blockDir.getLastBlockHash(), latestBlock->getHash());
}

// ============================================================================
// Error Cases
// ============================================================================

TEST_F(BlockDirTest, MoveFrontFileWithNoFiles) {
    blockDir.init(config, true);
    
    pp::BlockDir target;
    std::string targetDir = testDir + "/target";
    std::filesystem::create_directories(targetDir);
    pp::BlockDir::Config targetConfig(targetDir, 1024);
    target.init(targetConfig, true);
    
    // Try to move when no files exist (no blocks added)
    auto result = blockDir.moveFrontFileTo(target);
    EXPECT_TRUE(result.isError());
}

TEST_F(BlockDirTest, BlockchainOperationsWithoutManagement) {
    blockDir.init(config, false); // manageBlockchain = false
    
    // These should return safe defaults or nullptr
    EXPECT_EQ(blockDir.getBlockchainSize(), 0);
    EXPECT_EQ(blockDir.getLatestBlock(), nullptr);
    EXPECT_FALSE(blockDir.isBlockchainValid());
    EXPECT_EQ(blockDir.getLastBlockHash(), "0");
    
    // addBlock should fail
    auto block = createTestBlock(0);
    bool result = blockDir.addBlock(block);
    EXPECT_FALSE(result);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(BlockDirTest, HandlesLargeBlocks) {
    blockDir.init(config, true);
    
    // Create a block with large data (but smaller than max file size)
    size_t largeSize = 512 * 1024; // 512KB
    std::string largeData(largeSize, 'X');
    
    auto block = createTestBlock(0, largeData);
    bool result = blockDir.addBlock(block);
    EXPECT_TRUE(result);
    EXPECT_EQ(blockDir.getBlockchainSize(), 1);
    EXPECT_GT(blockDir.getTotalStorageSize(), 0);
}

TEST_F(BlockDirTest, HandlesManyBlocks) {
    blockDir.init(config, true);
    
    // Add many small blocks
    const size_t numBlocks = 100;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            block->setHash(block->calculateHash());
        }
        bool result = blockDir.addBlock(block);
        ASSERT_TRUE(result) << "Failed to add block " << i;
    }
    
    // Verify all blocks exist
    EXPECT_EQ(blockDir.getBlockchainSize(), numBlocks);
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = blockDir.getBlock(i);
        ASSERT_NE(block, nullptr) << "Block " << i << " not found";
        EXPECT_EQ(block->getIndex(), i);
    }
}
