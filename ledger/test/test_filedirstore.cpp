#include "FileDirStore.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

class FileDirStoreTest : public ::testing::Test {
protected:
    std::string testDir;
    pp::FileDirStore fileDirStore;
    pp::FileDirStore::InitConfig config;
    
    void SetUp() override {
        fileDirStore.setLogger("filedirstore");
        testDir = "/tmp/pp-ledger-filedirstore-test";
        
        // Clean up test directory
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
        // Don't create the directory here - let init() do it
        
        config = pp::FileDirStore::InitConfig();
        config.dirPath = testDir;
        config.maxFileCount = 5;
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

TEST_F(FileDirStoreTest, InitializesSuccessfully) {
    auto result = fileDirStore.init(config);
    EXPECT_TRUE(result.isOk());
    
    // Directory should exist
    EXPECT_TRUE(std::filesystem::exists(testDir));
    EXPECT_EQ(fileDirStore.getBlockCount(), 0);
    
    // Should fail to init again on existing directory
    pp::FileDirStore fileDirStore2;
    auto result2 = fileDirStore2.init(config);
    EXPECT_TRUE(result2.isError());
}

TEST_F(FileDirStoreTest, FailsWithInvalidConfig) {
    pp::FileDirStore::InitConfig invalidConfig;
    invalidConfig.dirPath = testDir;
    invalidConfig.maxFileCount = 0; // Invalid
    invalidConfig.maxFileSize = 1024 * 1024;
    
    auto result = fileDirStore.init(invalidConfig);
    EXPECT_TRUE(result.isError());
}

TEST_F(FileDirStoreTest, FailsWithSmallFileSize) {
    pp::FileDirStore::InitConfig smallConfig;
    smallConfig.dirPath = testDir;
    smallConfig.maxFileCount = 5;
    smallConfig.maxFileSize = 100; // Too small (must be at least 1MB)
    
    auto result = fileDirStore.init(smallConfig);
    EXPECT_TRUE(result.isError());
}

TEST_F(FileDirStoreTest, LoadsExistingIndex) {
    // First initialization and add blocks
    fileDirStore.init(config);
    auto block1 = createTestBlock(0);
    auto result1 = fileDirStore.appendBlock(block1);
    ASSERT_TRUE(result1.isOk());
    
    // Mount existing directory - should load existing index
    pp::FileDirStore fileDirStore2;
    fileDirStore2.setLogger("filedirstore2");
    auto result = fileDirStore2.mount(config.dirPath, config.maxFileCount, config.maxFileSize);
    EXPECT_TRUE(result.isOk());
    
    // Should have the block
    EXPECT_EQ(fileDirStore2.getBlockCount(), 1);
    auto readResult = fileDirStore2.readBlock(0);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), block1);
}

// ============================================================================
// Block Writing Tests
// ============================================================================

TEST_F(FileDirStoreTest, WritesSingleBlock) {
    fileDirStore.init(config);
    
    std::string blockData = "Test block data";
    auto result = fileDirStore.appendBlock(blockData);
    
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 0);
    EXPECT_EQ(fileDirStore.getBlockCount(), 1);
}

TEST_F(FileDirStoreTest, WritesMultipleBlocks) {
    fileDirStore.init(config);
    
    const size_t numBlocks = 10;
    std::vector<std::string> blockData;
    
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = createTestBlock(i);
        blockData.push_back(data);
        auto result = fileDirStore.appendBlock(data);
        ASSERT_TRUE(result.isOk()) << "Failed to add block " << i;
        EXPECT_EQ(result.value(), i);
    }
    
    EXPECT_EQ(fileDirStore.getBlockCount(), numBlocks);
    
    // Verify all blocks can be read back
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = fileDirStore.readBlock(i);
        ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i;
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}

TEST_F(FileDirStoreTest, CreatesNewFileWhenMaxSizeReached) {
    // Use small max file size to trigger file rollover
    config.maxFileSize = 1024 * 1024; // 1MB
    fileDirStore.init(config);
    
    // Add blocks with large data to trigger file rollover
    // Each block will be ~200KB, so we should get ~5 blocks per file
    std::string largeData(200 * 1024, 'X'); // 200KB per block
    
    const size_t numBlocks = 15; // Enough to trigger multiple files
    
    for (size_t i = 0; i < numBlocks; i++) {
        auto result = fileDirStore.appendBlock(largeData);
        ASSERT_TRUE(result.isOk()) << "Failed to add block " << i;
    }
    
    EXPECT_EQ(fileDirStore.getBlockCount(), numBlocks);
    
    // Verify index file exists
    std::string indexFile = testDir + "/idx.dat";
    EXPECT_TRUE(std::filesystem::exists(indexFile));
    
    // Verify multiple block files exist
    size_t fileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(testDir)) {
        if (entry.path().extension() == ".dat" && entry.path().filename() != "idx.dat") {
            fileCount++;
        }
    }
    EXPECT_GT(fileCount, 1); // Should have multiple files
}

TEST_F(FileDirStoreTest, StopsAtMaxFileCount) {
    config.maxFileCount = 3;
    config.maxFileSize = 1024 * 1024; // 1MB
    fileDirStore.init(config);
    
    // Add blocks until we reach max file count
    std::string largeData(200 * 1024, 'X'); // 200KB per block
    
    size_t blocksAdded = 0;
    for (size_t i = 0; i < 20; i++) { // Try to add more than max files allow
        auto result = fileDirStore.appendBlock(largeData);
        if (result.isOk()) {
            blocksAdded++;
        } else {
            // Should fail when max file count is reached
            break;
        }
    }
    
    // Should have added some blocks but stopped at max file count
    EXPECT_GT(blocksAdded, 0);
    EXPECT_LE(blocksAdded, config.maxFileCount * 5); // Rough estimate
}

// ============================================================================
// Block Reading Tests
// ============================================================================

TEST_F(FileDirStoreTest, ReadsBlockAcrossFiles) {
    config.maxFileSize = 1024 * 1024; // 1MB
    fileDirStore.init(config);
    
    // Add blocks that will span multiple files
    std::string largeData(200 * 1024, 'X');
    const size_t numBlocks = 10;
    
    std::vector<std::string> blockData;
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = largeData + std::to_string(i);
        blockData.push_back(data);
        auto result = fileDirStore.appendBlock(data);
        ASSERT_TRUE(result.isOk());
    }
    
    // Read blocks from different files
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = fileDirStore.readBlock(i);
        ASSERT_TRUE(readResult.isOk()) << "Failed to read block " << i;
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}

TEST_F(FileDirStoreTest, ReadBlockOutOfRange) {
    fileDirStore.init(config);
    
    // Try to read from empty store
    auto result = fileDirStore.readBlock(0);
    EXPECT_TRUE(result.isError());
    
    // Add one block
    fileDirStore.appendBlock("test");
    
    // Try to read out of range
    auto result2 = fileDirStore.readBlock(1);
    EXPECT_TRUE(result2.isError());
}

// ============================================================================
// CanFit Tests
// ============================================================================

TEST_F(FileDirStoreTest, CanFitChecksCorrectly) {
    fileDirStore.init(config);
    
    // Should be able to fit data in first file
    EXPECT_TRUE(fileDirStore.canFit(100));
    EXPECT_TRUE(fileDirStore.canFit(config.maxFileSize / 2));
    
    // Should not be able to fit data larger than file size
    EXPECT_FALSE(fileDirStore.canFit(config.maxFileSize + 1));
}

TEST_F(FileDirStoreTest, CanFitRespectsMaxFileCount) {
    config.maxFileCount = 2;
    fileDirStore.init(config);
    
    // Fill up files
    std::string largeData(200 * 1024, 'X');
    for (size_t i = 0; i < 10; i++) {
        auto result = fileDirStore.appendBlock(largeData);
        if (!result.isOk()) {
            break;
        }
    }
    
    // Once max file count is reached, canFit should return false
    EXPECT_FALSE(fileDirStore.canFit(100));
}

// ============================================================================
// Rewind Tests
// ============================================================================

TEST_F(FileDirStoreTest, RewindToIndex) {
    fileDirStore.init(config);
    
    // Add multiple blocks
    const size_t numBlocks = 10;
    for (size_t i = 0; i < numBlocks; i++) {
        auto result = fileDirStore.appendBlock(createTestBlock(i));
        ASSERT_TRUE(result.isOk());
    }
    
    EXPECT_EQ(fileDirStore.getBlockCount(), numBlocks);
    
    // Rewind to middle
    auto rewindResult = fileDirStore.rewindTo(5);
    ASSERT_TRUE(rewindResult.isOk());
    EXPECT_EQ(fileDirStore.getBlockCount(), 5);
    
    // Verify blocks before rewind point are still readable
    for (size_t i = 0; i < 5; i++) {
        auto readResult = fileDirStore.readBlock(i);
        ASSERT_TRUE(readResult.isOk());
    }
    
    // Verify blocks after rewind point are gone
    auto readResult = fileDirStore.readBlock(5);
    EXPECT_TRUE(readResult.isError());
}

TEST_F(FileDirStoreTest, RewindToZero) {
    fileDirStore.init(config);
    
    // Add blocks
    for (size_t i = 0; i < 5; i++) {
        fileDirStore.appendBlock(createTestBlock(i));
    }
    
    // Rewind to zero
    auto rewindResult = fileDirStore.rewindTo(0);
    ASSERT_TRUE(rewindResult.isOk());
    EXPECT_EQ(fileDirStore.getBlockCount(), 0);
}

TEST_F(FileDirStoreTest, RewindOutOfRange) {
    fileDirStore.init(config);
    
    fileDirStore.appendBlock("test");
    
    // Try to rewind beyond block count
    auto rewindResult = fileDirStore.rewindTo(10);
    EXPECT_TRUE(rewindResult.isError());
}

// ============================================================================
// Persistence Tests
// ============================================================================

TEST_F(FileDirStoreTest, PersistsAcrossRestarts) {
    // First session
    fileDirStore.init(config);
    const size_t numBlocks = 5;
    std::vector<std::string> blockData;
    
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = createTestBlock(i);
        blockData.push_back(data);
        fileDirStore.appendBlock(data);
    }
    
    // Second session - mount existing directory
    pp::FileDirStore fileDirStore2;
    fileDirStore2.setLogger("filedirstore2");
    fileDirStore2.mount(config.dirPath, config.maxFileCount, config.maxFileSize);
    
    EXPECT_EQ(fileDirStore2.getBlockCount(), numBlocks);
    
    // Verify all blocks are readable
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = fileDirStore2.readBlock(i);
        ASSERT_TRUE(readResult.isOk());
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}

TEST_F(FileDirStoreTest, PersistsMultipleFiles) {
    config.maxFileSize = 1024 * 1024; // 1MB
    fileDirStore.init(config);
    
    // Add blocks that span multiple files
    std::string largeData(200 * 1024, 'X');
    const size_t numBlocks = 10;
    std::vector<std::string> blockData;
    
    for (size_t i = 0; i < numBlocks; i++) {
        std::string data = largeData + std::to_string(i);
        blockData.push_back(data);
        fileDirStore.appendBlock(data);
    }
    
    // Mount existing directory
    pp::FileDirStore fileDirStore2;
    fileDirStore2.setLogger("filedirstore2");
    fileDirStore2.mount(config.dirPath, config.maxFileCount, config.maxFileSize);
    
    EXPECT_EQ(fileDirStore2.getBlockCount(), numBlocks);
    
    // Verify all blocks across files are readable
    for (size_t i = 0; i < numBlocks; i++) {
        auto readResult = fileDirStore2.readBlock(i);
        ASSERT_TRUE(readResult.isOk());
        EXPECT_EQ(readResult.value(), blockData[i]);
    }
}
