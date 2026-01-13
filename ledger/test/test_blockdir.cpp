#include "BlockDir.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>

class BlockDirTest : public ::testing::Test {
protected:
    std::string testDir;
    pp::BlockDir* blockDir;
    pp::BlockDir::Config* config;
    
    void SetUp() override {
        testDir = "/tmp/pp-ledger-blockdir-test";
        
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
        
        blockDir = new pp::BlockDir();
        config = new pp::BlockDir::Config(testDir, 100); // Very small max file size
    }
    
    void TearDown() override {
        delete blockDir;
        delete config;
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
    }
};

TEST_F(BlockDirTest, InitializesSuccessfully) {
    auto result = blockDir->init(*config);
    EXPECT_TRUE(result.isOk());
}

TEST_F(BlockDirTest, WritesBlock) {
    blockDir->init(*config);
    
    const char* blockData = "Block #1: First block of data";
    size_t blockSize = strlen(blockData) + 1;
    uint64_t blockId = 1001;
    
    auto result = blockDir->writeBlock(blockId, blockData, blockSize);
    EXPECT_TRUE(result.isOk());
}

TEST_F(BlockDirTest, ReadsBlockBack) {
    blockDir->init(*config);
    
    const char* blockData = "Test block data";
    size_t blockSize = strlen(blockData) + 1;
    uint64_t blockId = 1001;
    
    blockDir->writeBlock(blockId, blockData, blockSize);
    
    char readBuffer[256] = {0};
    auto result = blockDir->readBlock(blockId, readBuffer, sizeof(readBuffer));
    ASSERT_TRUE(result.isOk());
    EXPECT_STREQ(readBuffer, blockData);
}

TEST_F(BlockDirTest, HasBlockReturnsTrueForExistingBlock) {
    blockDir->init(*config);
    
    const char* blockData = "Block data";
    uint64_t blockId = 1001;
    
    blockDir->writeBlock(blockId, blockData, strlen(blockData) + 1);
    EXPECT_TRUE(blockDir->hasBlock(blockId));
}

TEST_F(BlockDirTest, HasBlockReturnsFalseForNonExistentBlock) {
    blockDir->init(*config);
    EXPECT_FALSE(blockDir->hasBlock(9999));
}

TEST_F(BlockDirTest, RejectsDuplicateBlock) {
    blockDir->init(*config);
    
    const char* blockData = "Block data";
    size_t blockSize = strlen(blockData) + 1;
    uint64_t blockId = 1001;
    
    auto result1 = blockDir->writeBlock(blockId, blockData, blockSize);
    EXPECT_TRUE(result1.isOk());
    
    auto result2 = blockDir->writeBlock(blockId, blockData, blockSize);
    EXPECT_TRUE(result2.isError());
}

TEST_F(BlockDirTest, FlushSucceeds) {
    blockDir->init(*config);
    EXPECT_NO_THROW(blockDir->flush());
}

TEST_F(BlockDirTest, PersistsDataAfterReopen) {
    blockDir->init(*config);
    
    const char* blockData = "Persistent block";
    size_t blockSize = strlen(blockData) + 1;
    uint64_t blockId = 1001;
    
    blockDir->writeBlock(blockId, blockData, blockSize);
    blockDir->flush();
    
    delete blockDir;
    blockDir = new pp::BlockDir();
    auto result = blockDir->init(*config);
    ASSERT_TRUE(result.isOk());
    
    EXPECT_TRUE(blockDir->hasBlock(blockId));
    
    char readBuffer[256] = {0};
    auto readResult = blockDir->readBlock(blockId, readBuffer, sizeof(readBuffer));
    ASSERT_TRUE(readResult.isOk());
    EXPECT_STREQ(readBuffer, blockData);
}

TEST_F(BlockDirTest, ReadNonExistentBlockFails) {
    blockDir->init(*config);
    
    char buffer[256];
    auto result = blockDir->readBlock(99999, buffer, sizeof(buffer));
    EXPECT_TRUE(result.isError());
}

TEST_F(BlockDirTest, MultipleBlocks) {
    blockDir->init(*config);
    
    const char* data1 = "Block #1";
    const char* data2 = "Block #2";
    const char* data3 = "Block #3";
    
    blockDir->writeBlock(1001, data1, strlen(data1) + 1);
    blockDir->writeBlock(1002, data2, strlen(data2) + 1);
    blockDir->writeBlock(1003, data3, strlen(data3) + 1);
    
    EXPECT_TRUE(blockDir->hasBlock(1001));
    EXPECT_TRUE(blockDir->hasBlock(1002));
    EXPECT_TRUE(blockDir->hasBlock(1003));
}
