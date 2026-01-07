#include "BlockFile.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>

class BlockFileTest : public ::testing::Test {
protected:
    std::string testDir;
    std::string testFile;
    pp::BlockFile* blockFile;
    pp::BlockFile::Config* config;
    
    void SetUp() override {
        testDir = "/tmp/pp-ledger-test";
        testFile = testDir + "/test_block.dat";
        
        if (std::filesystem::exists(testFile)) {
            std::filesystem::remove(testFile);
        }
        if (!std::filesystem::exists(testDir)) {
            std::filesystem::create_directories(testDir);
        }
        
        blockFile = new pp::BlockFile();
        config = new pp::BlockFile::Config(testFile, 1024 * 1024); // 1MB max size
    }
    
    void TearDown() override {
        delete blockFile;
        delete config;
        if (std::filesystem::exists(testFile)) {
            std::filesystem::remove(testFile);
        }
    }
};

TEST_F(BlockFileTest, InitializesSuccessfully) {
    auto result = blockFile->init(*config);
    EXPECT_TRUE(result.isOk());
}

TEST_F(BlockFileTest, WritesData) {
    blockFile->init(*config);
    
    const char* testData = "Hello, BlockFile!";
    size_t dataSize = strlen(testData) + 1;
    
    auto result = blockFile->write(testData, dataSize);
    ASSERT_TRUE(result.isOk());
    EXPECT_GE(result.value(), 0);
}

TEST_F(BlockFileTest, ReadsDataBack) {
    blockFile->init(*config);
    
    const char* testData = "Hello, BlockFile!";
    size_t dataSize = strlen(testData) + 1;
    
    auto writeResult = blockFile->write(testData, dataSize);
    ASSERT_TRUE(writeResult.isOk());
    
    int64_t offset = writeResult.value();
    char readBuffer[256] = {0};
    
    auto readResult = blockFile->read(offset, readBuffer, dataSize);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_STREQ(readBuffer, testData);
}

TEST_F(BlockFileTest, MultipleWrites) {
    blockFile->init(*config);
    
    const char* data1 = "First block";
    const char* data2 = "Second block";
    size_t size1 = strlen(data1) + 1;
    size_t size2 = strlen(data2) + 1;
    
    auto result1 = blockFile->write(data1, size1);
    auto result2 = blockFile->write(data2, size2);
    
    ASSERT_TRUE(result1.isOk());
    ASSERT_TRUE(result2.isOk());
    EXPECT_NE(result1.value(), result2.value());
}

TEST_F(BlockFileTest, CanFitReturnsFalseForOversizedData) {
    blockFile->init(*config);
    
    size_t hugeSize = 2 * 1024 * 1024; // 2MB (larger than max)
    EXPECT_FALSE(blockFile->canFit(hugeSize));
}

TEST_F(BlockFileTest, FlushSucceeds) {
    blockFile->init(*config);
    EXPECT_NO_THROW(blockFile->flush());
}

TEST_F(BlockFileTest, ReopensPersistentFile) {
    blockFile->init(*config);
    
    const char* testData = "Persistent data";
    size_t dataSize = strlen(testData) + 1;
    auto writeResult = blockFile->write(testData, dataSize);
    ASSERT_TRUE(writeResult.isOk());
    int64_t offset = writeResult.value();
    
    delete blockFile;
    blockFile = new pp::BlockFile();
    auto reopenResult = blockFile->init(*config);
    ASSERT_TRUE(reopenResult.isOk());
    
    char readBuffer[256] = {0};
    auto readResult = blockFile->read(offset, readBuffer, dataSize);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_STREQ(readBuffer, testData);
}
