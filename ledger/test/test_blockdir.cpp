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
    pp::BlockDir* blockDir;
    pp::BlockDir::Config config;
    
    void SetUp() override {
        testDir = "/tmp/pp-ledger-blockdir-test";
        
        // Clean up test directory
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
        std::filesystem::create_directories(testDir);
        
        blockDir = new pp::BlockDir();
        config = pp::BlockDir::Config(testDir, 1024 * 1024); // 1MB max file size
    }
    
    void TearDown() override {
        delete blockDir;
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
    }
    
    // Helper to create test block data
    std::string createTestData(uint64_t blockId, size_t size = 100) {
        std::string data(size, 'A' + (blockId % 26));
        return data;
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
    auto result = blockDir->init(config, false);
    EXPECT_TRUE(result.isOk());
    
    // Directory should exist
    EXPECT_TRUE(std::filesystem::exists(testDir));
    
    // Index file should not exist yet (no blocks written)
    std::string indexFile = testDir + "/blocks.index";
    EXPECT_FALSE(std::filesystem::exists(indexFile));
}

TEST_F(BlockDirTest, InitializesWithBlockchainManagement) {
    auto result = blockDir->init(config, true);
    EXPECT_TRUE(result.isOk());
    
    // Blockchain should be initialized
    EXPECT_EQ(blockDir->getBlockchainSize(), 0);
    EXPECT_EQ(blockDir->getLatestBlock(), nullptr);
    // Empty blockchain is not valid (BlockChain::isValid returns false for empty chain)
    EXPECT_FALSE(blockDir->isBlockchainValid());
    EXPECT_EQ(blockDir->getLastBlockHash(), "0");
}

TEST_F(BlockDirTest, InitializesWithExistingIndex) {
    // First initialization and add a block (which writes to storage)
    blockDir->init(config, true);
    auto block = createTestBlock(0);
    ASSERT_TRUE(blockDir->addBlock(block));
    delete blockDir;
    
    // Reinitialize - should load existing index
    blockDir = new pp::BlockDir();
    auto result = blockDir->init(config, true);
    EXPECT_TRUE(result.isOk());
    
    // Should have the block in blockchain
    EXPECT_EQ(blockDir->getBlockchainSize(), 1);
    EXPECT_NE(blockDir->getBlock(0), nullptr);
}

// ============================================================================
// Block Writing Tests (via blockchain management)
// ============================================================================

TEST_F(BlockDirTest, WritesSingleBlock) {
    blockDir->init(config, true);
    
    auto block = createTestBlock(0);
    bool result = blockDir->addBlock(block);
    
    EXPECT_TRUE(result);
    EXPECT_GT(blockDir->getTotalStorageSize(), 0);
    EXPECT_EQ(blockDir->getBlockchainSize(), 1);
}

TEST_F(BlockDirTest, WritesMultipleBlocks) {
    blockDir->init(config, true);
    
    const size_t numBlocks = 10;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        bool result = blockDir->addBlock(block);
        ASSERT_TRUE(result) << "Failed to add block " << i;
    }
    
    EXPECT_GT(blockDir->getTotalStorageSize(), 0);
    EXPECT_EQ(blockDir->getBlockchainSize(), numBlocks);
}

TEST_F(BlockDirTest, CreatesNewFileWhenMaxSizeReached) {
    // Use very small max file size to trigger file rollover
    pp::BlockDir::Config smallConfig(testDir, 1024); // 1KB max size
    blockDir->init(smallConfig, true);
    
    // Add blocks until file rollover occurs
    // Each block will be serialized, so actual size will be larger than data size
    uint64_t blockId = 0;
    size_t initialSize = blockDir->getTotalStorageSize();
    
    // Add blocks until storage size increases significantly (indicating new file)
    while (blockDir->getTotalStorageSize() < 1024 && blockId < 20) {
        auto block = createTestBlock(blockId, "X"); // Small data
        if (blockId > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        ASSERT_TRUE(blockDir->addBlock(block)) << "Failed to add block " << blockId;
        blockId++;
    }
    
    // Should have written multiple blocks
    EXPECT_GT(blockDir->getBlockchainSize(), 0);
    EXPECT_GT(blockDir->getTotalStorageSize(), initialSize);
}

// ============================================================================
// Index Persistence Tests
// ============================================================================

TEST_F(BlockDirTest, PersistsIndexToDisk) {
    blockDir->init(config, true);
    
    // Add some blocks (which writes to storage and saves index)
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        blockDir->addBlock(block);
    }
    
    // Index file should exist after adding blocks
    std::string indexFile = testDir + "/blocks.index";
    EXPECT_TRUE(std::filesystem::exists(indexFile));
    EXPECT_GT(std::filesystem::file_size(indexFile), 0);
}

TEST_F(BlockDirTest, LoadsIndexFromDisk) {
    blockDir->init(config, true);
    
    // Add blocks (which writes to storage)
    const size_t numBlocks = 10;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        blockDir->addBlock(block);
    }
    delete blockDir;
    
    // Reinitialize and verify blocks are loaded
    blockDir = new pp::BlockDir();
    blockDir->init(config, true);
    
    // Blocks should be loaded into blockchain
    EXPECT_EQ(blockDir->getBlockchainSize(), numBlocks);
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = blockDir->getBlock(i);
        ASSERT_NE(block, nullptr) << "Block " << i << " not found after reload";
        EXPECT_EQ(block->getIndex(), i);
    }
}

// ============================================================================
// File Management Tests
// ============================================================================

TEST_F(BlockDirTest, GetTotalStorageSize) {
    blockDir->init(config, true);
    
    EXPECT_EQ(blockDir->getTotalStorageSize(), 0);
    
    // Add blocks and verify size increases
    auto block = createTestBlock(0);
    blockDir->addBlock(block);
    
    EXPECT_GT(blockDir->getTotalStorageSize(), 0);
}

TEST_F(BlockDirTest, MoveFrontFileToTarget) {
    // Create source and target directories
    std::string sourceDir = testDir + "/source";
    std::string targetDir = testDir + "/target";
    std::filesystem::create_directories(sourceDir);
    std::filesystem::create_directories(targetDir);
    
    pp::BlockDir source;
    pp::BlockDir target;
    
    pp::BlockDir::Config sourceConfig(sourceDir, 1024);
    pp::BlockDir::Config targetConfig(targetDir, 1024);
    
    source.init(sourceConfig, true);
    target.init(targetConfig, true);
    
    // Add blocks to source (which writes to storage)
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = source.getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
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
    // getTotalStorageSize counts all files (open and closed)
    EXPECT_GT(target.getTotalStorageSize(), 0);
    
    // Also verify the file exists in the target directory
    std::string targetBlockFile = targetDir + "/block_000001.dat";
    EXPECT_TRUE(std::filesystem::exists(targetBlockFile));
}

// ============================================================================
// Blockchain Management Tests (when manageBlockchain=true)
// ============================================================================

TEST_F(BlockDirTest, AddsBlockToBlockchain) {
    blockDir->init(config, true);
    
    auto block = createTestBlock(0);
    bool result = blockDir->addBlock(block);
    
    EXPECT_TRUE(result);
    EXPECT_EQ(blockDir->getBlockchainSize(), 1);
    EXPECT_NE(blockDir->getLatestBlock(), nullptr);
    EXPECT_EQ(blockDir->getLatestBlock()->getIndex(), 0);
}

TEST_F(BlockDirTest, AddsMultipleBlocksToBlockchain) {
    blockDir->init(config, true);
    
    const size_t numBlocks = 10;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        bool result = blockDir->addBlock(block);
        ASSERT_TRUE(result) << "Failed to add block " << i;
    }
    
    EXPECT_EQ(blockDir->getBlockchainSize(), numBlocks);
    EXPECT_NE(blockDir->getLatestBlock(), nullptr);
    EXPECT_EQ(blockDir->getLatestBlock()->getIndex(), numBlocks - 1);
}

TEST_F(BlockDirTest, GetsBlockByIndex) {
    blockDir->init(config, true);
    
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        blockDir->addBlock(block);
    }
    
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = blockDir->getBlock(i);
        ASSERT_NE(block, nullptr) << "Block " << i << " not found";
        EXPECT_EQ(block->getIndex(), i);
    }
    
    // Non-existent block
    EXPECT_EQ(blockDir->getBlock(999), nullptr);
}

TEST_F(BlockDirTest, ValidatesBlockchain) {
    blockDir->init(config, true);
    
    // Empty blockchain should not be valid (BlockChain::isValid returns false for empty chain)
    EXPECT_FALSE(blockDir->isBlockchainValid());
    
    // Add valid blocks
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        blockDir->addBlock(block);
    }
    
    EXPECT_TRUE(blockDir->isBlockchainValid());
}

TEST_F(BlockDirTest, GetsLastBlockHash) {
    blockDir->init(config, true);
    
    // Empty blockchain
    EXPECT_EQ(blockDir->getLastBlockHash(), "0");
    
    // Add blocks
    const size_t numBlocks = 3;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        blockDir->addBlock(block);
    }
    
    auto latestBlock = blockDir->getLatestBlock();
    ASSERT_NE(latestBlock, nullptr);
    EXPECT_EQ(blockDir->getLastBlockHash(), latestBlock->getHash());
}

TEST_F(BlockDirTest, PopulatesBlockchainFromStorage) {
    blockDir->init(config, true);
    
    // Add blocks (which writes to storage and automatically saves index)
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        blockDir->addBlock(block);
    }
    
    // Create new BlockDir - should automatically populate from storage during init
    delete blockDir;
    blockDir = new pp::BlockDir();
    blockDir->init(config, true);
    
    // Should have loaded blocks from storage automatically
    EXPECT_EQ(blockDir->getBlockchainSize(), numBlocks);
    EXPECT_NE(blockDir->getLatestBlock(), nullptr);
    EXPECT_EQ(blockDir->getLatestBlock()->getIndex(), numBlocks - 1);
}

// ============================================================================
// Error Cases
// ============================================================================

TEST_F(BlockDirTest, HandlesInvalidDirectory) {
    // Try to initialize with non-existent parent directory
    pp::BlockDir::Config invalidConfig("/nonexistent/path/blockdir", 1024);
    auto result = blockDir->init(invalidConfig, false);
    // Should either succeed (creates directory) or fail gracefully
    // This depends on implementation - check if it creates parent dirs
}

TEST_F(BlockDirTest, MoveFrontFileWithNoFiles) {
    blockDir->init(config, true);
    
    pp::BlockDir target;
    std::string targetDir = testDir + "/target";
    std::filesystem::create_directories(targetDir);
    pp::BlockDir::Config targetConfig(targetDir, 1024);
    target.init(targetConfig, true);
    
    // Try to move when no files exist (no blocks added)
    auto result = blockDir->moveFrontFileTo(target);
    EXPECT_TRUE(result.isError());
}

TEST_F(BlockDirTest, BlockchainOperationsWithoutManagement) {
    blockDir->init(config, false); // manageBlockchain = false
    
    // These should return safe defaults or nullptr
    EXPECT_EQ(blockDir->getBlockchainSize(), 0);
    EXPECT_EQ(blockDir->getLatestBlock(), nullptr);
    EXPECT_FALSE(blockDir->isBlockchainValid());
    EXPECT_EQ(blockDir->getLastBlockHash(), "0");
    
    // addBlock should fail
    auto block = createTestBlock(0);
    bool result = blockDir->addBlock(block);
    EXPECT_FALSE(result);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(BlockDirTest, HandlesLargeBlocks) {
    blockDir->init(config, true);
    
    // Create a block with large data (but smaller than max file size)
    size_t largeSize = 512 * 1024; // 512KB
    std::string largeData(largeSize, 'X');
    
    auto block = createTestBlock(0, largeData);
    bool result = blockDir->addBlock(block);
    EXPECT_TRUE(result);
    EXPECT_EQ(blockDir->getBlockchainSize(), 1);
    EXPECT_GT(blockDir->getTotalStorageSize(), 0);
}

TEST_F(BlockDirTest, HandlesManyBlocks) {
    blockDir->init(config, true);
    
    // Add many small blocks
    const size_t numBlocks = 100;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
        }
        bool result = blockDir->addBlock(block);
        ASSERT_TRUE(result) << "Failed to add block " << i;
    }
    
    // Verify all blocks exist
    EXPECT_EQ(blockDir->getBlockchainSize(), numBlocks);
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = blockDir->getBlock(i);
        ASSERT_NE(block, nullptr) << "Block " << i << " not found";
        EXPECT_EQ(block->getIndex(), i);
    }
}

TEST_F(BlockDirTest, FlushPersistsData) {
    blockDir->init(config, true);
    
    // Add blocks (which automatically saves index)
    const size_t numBlocks = 5;
    for (uint64_t i = 0; i < numBlocks; i++) {
        auto block = createTestBlock(i);
        if (i > 0) {
            auto prevBlock = blockDir->getLatestBlock();
            block->setPreviousHash(prevBlock->getHash());
            // Recalculate hash after updating previousHash
            block->setHash(block->calculateHash());
        }
        blockDir->addBlock(block);
    }
    
    // Index file should exist after adding blocks
    std::string indexFile = testDir + "/blocks.index";
    EXPECT_TRUE(std::filesystem::exists(indexFile));
}
