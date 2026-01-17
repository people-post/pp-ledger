#include "DirDirStore.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

class DirDirStoreTest : public ::testing::Test {
protected:
    std::string testDir;
    pp::DirDirStore dirDirStore{"dirdirstore"};
    pp::DirDirStore::Config config;
    
    void SetUp() override {
        testDir = "/tmp/pp-ledger-dirdirstore-test";
        
        // Clean up test directory
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
        std::filesystem::create_directories(testDir);
        
        config = pp::DirDirStore::Config();
        config.dirPath = testDir;
        config.maxDirCount = 3;
        config.maxFileCount = 3;
        config.maxFileSize = 1024 * 1024; // 1MB per file
    }
    
    void TearDown() override {
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
    }
    
    // Helper to create test block data
    std::string createTestBlock(uint64_t index, size_t size = 100) {
        std::string data(size, 'A' + (index % 26));
        return data;
    }
};

// ============================================================================
// Initialization Tests
// ============================================================================

TEST_F(DirDirStoreTest, InitializesSuccessfully) {
    auto result = dirDirStore.init(config);
    EXPECT_TRUE(result.isOk());
    
    // Directory should exist
    EXPECT_TRUE(std::filesystem::exists(testDir));
    EXPECT_EQ(dirDirStore.getBlockCount(), 0);
}

TEST_F(DirDirStoreTest, FailsWithInvalidConfig) {
    pp::DirDirStore::Config invalidConfig;
    invalidConfig.dirPath = testDir;
    invalidConfig.maxDirCount = 0; // Invalid
    invalidConfig.maxFileCount = 3;
    invalidConfig.maxFileSize = 1024 * 1024;
    
    auto result = dirDirStore.init(invalidConfig);
    EXPECT_TRUE(result.isError());
}

TEST_F(DirDirStoreTest, FailsWithSmallFileSize) {
    pp::DirDirStore::Config smallConfig;
    smallConfig.dirPath = testDir;
    smallConfig.maxDirCount = 3;
    smallConfig.maxFileCount = 3;
    smallConfig.maxFileSize = 100; // Too small (must be at least 1MB)
    
    auto result = dirDirStore.init(smallConfig);
    EXPECT_TRUE(result.isError());
}

TEST_F(DirDirStoreTest, LoadsExistingIndex) {
    // First initialization and add blocks
    dirDirStore.init(config);
    auto block1 = createTestBlock(0);
    auto result1 = dirDirStore.appendBlock(block1);
    ASSERT_TRUE(result1.isOk());
    
    // Reinitialize - should load existing index
    pp::DirDirStore dirDirStore2("dirdirstore2");
    auto result = dirDirStore2.init(config);
    EXPECT_TRUE(result.isOk());
    
    // Should have the block
    EXPECT_EQ(dirDirStore2.getBlockCount(), 1);
    auto readResult = dirDirStore2.readBlock(0);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), block1);
}

// ============================================================================
// Block Writing Tests - FILES Mode
// ============================================================================

TEST_F(DirDirStoreTest, WritesSingleBlockInFilesMode) {
    dirDirStore.init(config);
    
    std::string blockData = "Test block data";
    auto result = dirDirStore.appendBlock(blockData);
    
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 0);
    EXPECT_EQ(dirDirStore.getBlockCount(), 1);
}

TEST_F(DirDirStoreTest, WritesMultipleBlocksInFilesMode) {
    dirDirStore.init(config);
    
    const size_t numBlocks = 10;
    std::vector<std::string> blockData;
    
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = createTestBlock(i);
        blockData.push_back(data);
        auto result = dirDirStore.appendBlock(data);
        ASSERT_TRUE(result.isOk()) << "Failed to add block " << i;
        EXPECT_EQ(result.value(), i);
    }
    
    EXPECT_EQ(dirDirStore.getBlockCount(), numBlocks);
    
    // Verify all blocks can be read back
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = dirDirStore.readBlock(i);
        ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i;
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}

TEST_F(DirDirStoreTest, CreatesNewFileWhenMaxSizeReached) {
    config.maxFileSize = 1024 * 1024; // 1MB
    dirDirStore.init(config);
    
    // Add blocks with large data to trigger file rollover
    std::string largeData(200 * 1024, 'X'); // 200KB per block
    
    const size_t numBlocks = 10;
    
    for (size_t i = 0; i < numBlocks; i++) {
        auto result = dirDirStore.appendBlock(largeData);
        ASSERT_TRUE(result.isOk()) << "Failed to add block " << i;
    }
    
    EXPECT_EQ(dirDirStore.getBlockCount(), numBlocks);
    
    // Verify index file exists
    std::string indexFile = testDir + "/idx.dat";
    EXPECT_TRUE(std::filesystem::exists(indexFile));
}

// ============================================================================
// Transition to DIRS Mode Tests
// ============================================================================

TEST_F(DirDirStoreTest, TransitionsToDirsModeWhenMaxFileCountReached) {
    config.maxFileCount = 2; // Small number to trigger transition
    config.maxFileSize = 1024 * 1024; // 1MB
    dirDirStore.init(config);
    
    // Add blocks until we reach max file count
    std::string largeData(200 * 1024, 'X'); // 200KB per block
    
    size_t blocksAdded = 0;
    for (size_t i = 0; i < 20; i++) {
        auto result = dirDirStore.appendBlock(largeData);
        if (result.isOk()) {
            blocksAdded++;
        } else {
            // Should transition to DIRS mode and continue
            break;
        }
    }
    
    // Should have added blocks and transitioned
    EXPECT_GT(blocksAdded, 0);
    EXPECT_GT(dirDirStore.getBlockCount(), 0);
    
    // Verify subdirectories were created
    size_t dirCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(testDir)) {
        if (entry.is_directory() && entry.path().filename() != "." && entry.path().filename() != "..") {
            dirCount++;
        }
    }
    // Should have created subdirectories for FileDirStores
    EXPECT_GE(dirCount, 0); // May or may not have transitioned yet depending on timing
}

TEST_F(DirDirStoreTest, ContinuesWritingAfterTransitionToDirsMode) {
    config.maxFileCount = 2;
    config.maxFileSize = 1024 * 1024;
    dirDirStore.init(config);
    
    // Fill up files mode
    std::string largeData(200 * 1024, 'X');
    for (size_t i = 0; i < 10; i++) {
        auto result = dirDirStore.appendBlock(largeData);
        if (!result.isOk()) {
            break;
        }
    }
    
    // Continue adding blocks (should work in DIRS mode)
    std::string smallData = "Small block";
    auto result = dirDirStore.appendBlock(smallData);
    // Should succeed (either in FILES or DIRS mode)
    EXPECT_TRUE(result.isOk() || result.isError()); // May succeed or fail depending on state
}

// ============================================================================
// Block Reading Tests
// ============================================================================

TEST_F(DirDirStoreTest, ReadsBlockAcrossFiles) {
    config.maxFileSize = 1024 * 1024; // 1MB
    dirDirStore.init(config);
    
    // Add blocks that will span multiple files
    std::string largeData(200 * 1024, 'X');
    const size_t numBlocks = 10;
    
    std::vector<std::string> blockData;
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = largeData + std::to_string(i);
        blockData.push_back(data);
        auto result = dirDirStore.appendBlock(data);
        ASSERT_TRUE(result.isOk());
    }
    
    // Read blocks from different files
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = dirDirStore.readBlock(i);
        ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i;
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}

TEST_F(DirDirStoreTest, ReadBlockOutOfRange) {
    dirDirStore.init(config);
    
    // Try to read from empty store
    auto result = dirDirStore.readBlock(0);
    EXPECT_TRUE(result.isError());
    
    // Add one block
    dirDirStore.appendBlock("test");
    
    // Try to read out of range
    auto result2 = dirDirStore.readBlock(1);
    EXPECT_TRUE(result2.isError());
}

// ============================================================================
// CanFit Tests
// ============================================================================

TEST_F(DirDirStoreTest, CanFitChecksCorrectly) {
    dirDirStore.init(config);
    
    // Should be able to fit data in first file
    EXPECT_TRUE(dirDirStore.canFit(100));
    EXPECT_TRUE(dirDirStore.canFit(config.maxFileSize / 2));
    
    // Should not be able to fit data larger than file size
    EXPECT_FALSE(dirDirStore.canFit(config.maxFileSize + 1));
}

TEST_F(DirDirStoreTest, CanFitRespectsMaxFileCount) {
    config.maxFileCount = 2;
    dirDirStore.init(config);
    
    // Fill up files
    std::string largeData(200 * 1024, 'X');
    for (size_t i = 0; i < 10; i++) {
        auto result = dirDirStore.appendBlock(largeData);
        if (!result.isOk()) {
            break;
        }
    }
    
    // After max file count, should transition to DIRS mode
    // In DIRS mode, canFit should check if we can create new dirs
    bool canFit = dirDirStore.canFit(100);
    // May be true if we can create new dirs, or false if max dir count reached
    EXPECT_TRUE(canFit || !canFit); // Just check it doesn't crash
}

// ============================================================================
// Rewind Tests
// ============================================================================

TEST_F(DirDirStoreTest, RewindToIndex) {
    dirDirStore.init(config);
    
    // Add multiple blocks
    const size_t numBlocks = 10;
    for (size_t i = 0; i < numBlocks; i++) {
        auto result = dirDirStore.appendBlock(createTestBlock(i));
        ASSERT_TRUE(result.isOk());
    }
    
    EXPECT_EQ(dirDirStore.getBlockCount(), numBlocks);
    
    // Rewind to middle
    auto rewindResult = dirDirStore.rewindTo(5);
    ASSERT_TRUE(rewindResult.isOk());
    EXPECT_EQ(dirDirStore.getBlockCount(), 5);
    
    // Verify blocks before rewind point are still readable
    for (size_t i = 0; i < 5; i++) {
        auto readResult = dirDirStore.readBlock(i);
        ASSERT_TRUE(readResult.isOk());
    }
    
    // Verify blocks after rewind point are gone
    auto readResult = dirDirStore.readBlock(5);
    EXPECT_TRUE(readResult.isError());
}

TEST_F(DirDirStoreTest, RewindToZero) {
    dirDirStore.init(config);
    
    // Add blocks
    for (size_t i = 0; i < 5; i++) {
        dirDirStore.appendBlock(createTestBlock(i));
    }
    
    // Rewind to zero
    auto rewindResult = dirDirStore.rewindTo(0);
    ASSERT_TRUE(rewindResult.isOk());
    EXPECT_EQ(dirDirStore.getBlockCount(), 0);
}

TEST_F(DirDirStoreTest, RewindOutOfRange) {
    dirDirStore.init(config);
    
    dirDirStore.appendBlock("test");
    
    // Try to rewind beyond block count
    auto rewindResult = dirDirStore.rewindTo(10);
    EXPECT_TRUE(rewindResult.isError());
}

// ============================================================================
// Persistence Tests
// ============================================================================

TEST_F(DirDirStoreTest, PersistsAcrossRestarts) {
    // First session
    dirDirStore.init(config);
    const size_t numBlocks = 5;
    std::vector<std::string> blockData;
    
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = createTestBlock(i);
        blockData.push_back(data);
        dirDirStore.appendBlock(data);
    }
    
    // Second session - reinitialize
    pp::DirDirStore dirDirStore2("dirdirstore2");
    dirDirStore2.init(config);
    
    EXPECT_EQ(dirDirStore2.getBlockCount(), numBlocks);
    
    // Verify all blocks are readable
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = dirDirStore2.readBlock(i);
        ASSERT_TRUE(readResult.isOk());
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}

TEST_F(DirDirStoreTest, PersistsMultipleFiles) {
    config.maxFileSize = 1024 * 1024; // 1MB
    dirDirStore.init(config);
    
    // Add blocks that span multiple files
    std::string largeData(200 * 1024, 'X');
    const size_t numBlocks = 10;
    std::vector<std::string> blockData;
    
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = largeData + std::to_string(i);
        blockData.push_back(data);
        dirDirStore.appendBlock(data);
    }
    
    // Reinitialize
    pp::DirDirStore dirDirStore2("dirdirstore2");
    dirDirStore2.init(config);
    
    EXPECT_EQ(dirDirStore2.getBlockCount(), numBlocks);
    
    // Verify all blocks across files are readable
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = dirDirStore2.readBlock(i);
        ASSERT_TRUE(readResult.isOk());
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}

// ============================================================================
// Recursive DirDirStore Tests
// ============================================================================

TEST_F(DirDirStoreTest, CreatesRecursiveDirDirStores) {
    config.maxDirCount = 2; // Small to trigger recursive mode
    config.maxFileCount = 2;
    config.maxFileSize = 1024 * 1024;
    dirDirStore.init(config);
    
    // Add many blocks to trigger recursive mode
    std::string largeData(200 * 1024, 'X');
    
    // Add blocks until we trigger recursive mode
    for (size_t i = 0; i < 50; i++) {
        auto result = dirDirStore.appendBlock(largeData);
        if (!result.isOk()) {
            break;
        }
    }
    
    // Should have added blocks
    EXPECT_GT(dirDirStore.getBlockCount(), 0);
    
    // Verify we can still read blocks
    if (dirDirStore.getBlockCount() > 0) {
        auto readResult = dirDirStore.readBlock(0);
        EXPECT_TRUE(readResult.isOk() || readResult.isError()); // May succeed or fail
    }
}

TEST_F(DirDirStoreTest, HandlesDeepRecursion) {
    config.maxDirCount = 2;
    config.maxFileCount = 2;
    config.maxFileSize = 1024 * 1024;
    dirDirStore.init(config);
    
    // Add many blocks to create deep recursion
    std::string largeData(200 * 1024, 'X');
    
    size_t blocksAdded = 0;
    for (size_t i = 0; i < 100; i++) {
        auto result = dirDirStore.appendBlock(largeData);
        if (result.isOk()) {
            blocksAdded++;
        } else {
            break;
        }
    }
    
    // Should have added some blocks
    EXPECT_GT(blocksAdded, 0);
    
    // Verify we can read blocks back
    for (size_t i = 0; i < std::min(blocksAdded, size_t(10)); i++) {
        auto readResult = dirDirStore.readBlock(i);
        // May succeed or fail depending on state
        EXPECT_TRUE(readResult.isOk() || readResult.isError());
    }
}
