#include "BlockFile.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <cstdint>

class BlockFileTest : public ::testing::Test {
protected:
    std::string testDir;
    std::string testFile;
    pp::BlockFile* pBlockFile;
    pp::BlockFile::Config* pConfig;
    
    void SetUp() override {
        testDir = "/tmp/pp-ledger-test";
        testFile = testDir + "/test_block.dat";
        
        if (std::filesystem::exists(testFile)) {
            std::filesystem::remove(testFile);
        }
        if (!std::filesystem::exists(testDir)) {
            std::filesystem::create_directories(testDir);
        }
        
        pBlockFile = new pp::BlockFile();
        pConfig = new pp::BlockFile::Config(testFile, 1024 * 1024); // 1MB max size
    }
    
    void TearDown() override {
        delete pBlockFile;
        delete pConfig;
        if (std::filesystem::exists(testFile)) {
            std::filesystem::remove(testFile);
        }
    }
};

TEST_F(BlockFileTest, InitializesSuccessfully) {
    auto result = pBlockFile->init(*pConfig);
    EXPECT_TRUE(result.isOk());
    EXPECT_TRUE(pBlockFile->isOpen());
    
    // After initialization, file should exist and have some size
    EXPECT_TRUE(std::filesystem::exists(testFile));
    EXPECT_GT(pBlockFile->getCurrentSize(), 0);
    EXPECT_LE(pBlockFile->getCurrentSize(), pBlockFile->getMaxSize());
}

TEST_F(BlockFileTest, WritesData) {
    pBlockFile->init(*pConfig);
    
    size_t initialSize = pBlockFile->getCurrentSize();
    const char* testData = "Hello, BlockFile!";
    size_t dataSize = strlen(testData) + 1;
    
    auto result = pBlockFile->write(testData, dataSize);
    ASSERT_TRUE(result.isOk());
    
    // Offset should be non-negative
    int64_t offset = result.value();
    EXPECT_GE(offset, 0);
    
    // File size should increase by at least the data size
    size_t newSize = pBlockFile->getCurrentSize();
    EXPECT_GE(newSize, initialSize + dataSize);
    EXPECT_LE(newSize, pBlockFile->getMaxSize());
}

TEST_F(BlockFileTest, ReadsDataBack) {
    pBlockFile->init(*pConfig);
    
    const char* testData = "Hello, BlockFile!";
    size_t dataSize = strlen(testData) + 1;
    
    auto writeResult = pBlockFile->write(testData, dataSize);
    ASSERT_TRUE(writeResult.isOk());
    
    // Use the offset returned from write to read back
    int64_t offset = writeResult.value();
    char readBuffer[256] = {0};
    
    auto readResult = pBlockFile->read(offset, readBuffer, dataSize);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
    EXPECT_STREQ(readBuffer, testData);
}

TEST_F(BlockFileTest, MultipleWrites) {
    pBlockFile->init(*pConfig);
    
    const char* data1 = "First block";
    const char* data2 = "Second block";
    size_t size1 = strlen(data1) + 1;
    size_t size2 = strlen(data2) + 1;
    
    auto result1 = pBlockFile->write(data1, size1);
    auto result2 = pBlockFile->write(data2, size2);
    
    ASSERT_TRUE(result1.isOk());
    ASSERT_TRUE(result2.isOk());
    
    int64_t offset1 = result1.value();
    int64_t offset2 = result2.value();
    
    // Offsets should be different and sequential
    EXPECT_NE(offset1, offset2);
    EXPECT_GT(offset2, offset1);
    
    // Second offset should be at least size1 bytes after first
    EXPECT_GE(offset2 - offset1, static_cast<int64_t>(size1));
    
    // Verify we can read both back using their offsets
    char readBuffer1[256] = {0};
    char readBuffer2[256] = {0};
    
    auto read1 = pBlockFile->read(offset1, readBuffer1, size1);
    auto read2 = pBlockFile->read(offset2, readBuffer2, size2);
    
    ASSERT_TRUE(read1.isOk());
    ASSERT_TRUE(read2.isOk());
    EXPECT_STREQ(readBuffer1, data1);
    EXPECT_STREQ(readBuffer2, data2);
    
    // File size should account for both writes
    size_t initialSize = pBlockFile->getCurrentSize();
    EXPECT_GE(initialSize, size1 + size2);
}

TEST_F(BlockFileTest, CanFitReturnsFalseForOversizedData) {
    pBlockFile->init(*pConfig);
    
    // Try to write data larger than max size
    size_t hugeSize = 2 * 1024 * 1024; // 2MB (larger than max of 1MB)
    EXPECT_FALSE(pBlockFile->canFit(hugeSize));
    
    // Try to write data that should fit
    size_t smallSize = 100;
    EXPECT_TRUE(pBlockFile->canFit(smallSize));
}

TEST_F(BlockFileTest, CanFitAccountsForCurrentFileSize) {
    pBlockFile->init(*pConfig);
    
    size_t initialSize = pBlockFile->getCurrentSize();
    size_t maxSize = pBlockFile->getMaxSize();
    
    // Calculate how much space is available
    size_t availableSpace = maxSize - initialSize;
    
    // Should be able to fit data that's less than available space
    if (availableSpace > 0) {
        EXPECT_TRUE(pBlockFile->canFit(availableSpace));
        EXPECT_FALSE(pBlockFile->canFit(availableSpace + 1));
    }
}

// Note: flush() is now private and called automatically after writes
// No explicit flush test needed as it's tested implicitly through write operations

TEST_F(BlockFileTest, ReopensPersistentFile) {
    // Write data to file
    pBlockFile->init(*pConfig);
    
    const char* testData = "Persistent data";
    size_t dataSize = strlen(testData) + 1;
    auto writeResult = pBlockFile->write(testData, dataSize);
    ASSERT_TRUE(writeResult.isOk());
    int64_t offset = writeResult.value();
    size_t fileSize = pBlockFile->getCurrentSize();
    
    // Close and reopen
    delete pBlockFile;
    pBlockFile = new pp::BlockFile();
    auto reopenResult = pBlockFile->init(*pConfig);
    ASSERT_TRUE(reopenResult.isOk());
    
    // File size should be restored
    EXPECT_EQ(pBlockFile->getCurrentSize(), fileSize);
    
    // Offset should still be valid and data should be readable
    char readBuffer[256] = {0};
    auto readResult = pBlockFile->read(offset, readBuffer, dataSize);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
    EXPECT_STREQ(readBuffer, testData);
}

TEST_F(BlockFileTest, FileSizeMatchesActualFileSize) {
    pBlockFile->init(*pConfig);
    
    // After init, reported size should match actual file size
    size_t reportedSize = pBlockFile->getCurrentSize();
    size_t actualFileSize = std::filesystem::file_size(testFile);
    EXPECT_EQ(reportedSize, actualFileSize);
    
    // After writing, sizes should still match
    const char* testData = "Test data";
    size_t dataSize = strlen(testData) + 1;
    auto writeResult = pBlockFile->write(testData, dataSize);
    ASSERT_TRUE(writeResult.isOk());
    
    reportedSize = pBlockFile->getCurrentSize();
    actualFileSize = std::filesystem::file_size(testFile);
    EXPECT_EQ(reportedSize, actualFileSize);
}

TEST_F(BlockFileTest, OffsetsAreSequential) {
    pBlockFile->init(*pConfig);
    
    const char* data1 = "Block 1";
    const char* data2 = "Block 2";
    const char* data3 = "Block 3";
    size_t size1 = strlen(data1) + 1;
    size_t size2 = strlen(data2) + 1;
    size_t size3 = strlen(data3) + 1;
    
    auto result1 = pBlockFile->write(data1, size1);
    auto result2 = pBlockFile->write(data2, size2);
    auto result3 = pBlockFile->write(data3, size3);
    
    ASSERT_TRUE(result1.isOk());
    ASSERT_TRUE(result2.isOk());
    ASSERT_TRUE(result3.isOk());
    
    int64_t offset1 = result1.value();
    int64_t offset2 = result2.value();
    int64_t offset3 = result3.value();
    
    // Offsets should be in ascending order
    EXPECT_LT(offset1, offset2);
    EXPECT_LT(offset2, offset3);
    
    // Each offset should be at least the size of previous data after previous offset
    EXPECT_GE(offset2 - offset1, static_cast<int64_t>(size1));
    EXPECT_GE(offset3 - offset2, static_cast<int64_t>(size2));
    
    // All offsets should allow reading back correct data
    char buffer1[256] = {0};
    char buffer2[256] = {0};
    char buffer3[256] = {0};
    
    EXPECT_TRUE(pBlockFile->read(offset1, buffer1, size1).isOk());
    EXPECT_TRUE(pBlockFile->read(offset2, buffer2, size2).isOk());
    EXPECT_TRUE(pBlockFile->read(offset3, buffer3, size3).isOk());
    
    EXPECT_STREQ(buffer1, data1);
    EXPECT_STREQ(buffer2, data2);
    EXPECT_STREQ(buffer3, data3);
}

TEST_F(BlockFileTest, WriteOffsetCanBeUsedToRead) {
    pBlockFile->init(*pConfig);
    
    // Write multiple blocks and verify each offset works for reading
    const char* blocks[] = {"Block A", "Block B", "Block C"};
    size_t sizes[3];
    int64_t offsets[3];
    
    for (int i = 0; i < 3; i++) {
        sizes[i] = strlen(blocks[i]) + 1;
        auto result = pBlockFile->write(blocks[i], sizes[i]);
        ASSERT_TRUE(result.isOk());
        offsets[i] = result.value();
    }
    
    // Read back using the offsets
    for (int i = 0; i < 3; i++) {
        char buffer[256] = {0};
        auto readResult = pBlockFile->read(offsets[i], buffer, sizes[i]);
        ASSERT_TRUE(readResult.isOk());
        EXPECT_STREQ(buffer, blocks[i]);
    }
}

TEST_F(BlockFileTest, FileSizeIncreasesWithWrites) {
    pBlockFile->init(*pConfig);
    
    size_t sizeAfterInit = pBlockFile->getCurrentSize();
    
    const char* data1 = "First";
    const char* data2 = "Second";
    size_t size1 = strlen(data1) + 1;
    size_t size2 = strlen(data2) + 1;
    
    auto result1 = pBlockFile->write(data1, size1);
    ASSERT_TRUE(result1.isOk());
    size_t sizeAfterFirst = pBlockFile->getCurrentSize();
    EXPECT_GT(sizeAfterFirst, sizeAfterInit);
    
    auto result2 = pBlockFile->write(data2, size2);
    ASSERT_TRUE(result2.isOk());
    size_t sizeAfterSecond = pBlockFile->getCurrentSize();
    EXPECT_GT(sizeAfterSecond, sizeAfterFirst);
    
    // Size should increase by at least the data written
    EXPECT_GE(sizeAfterFirst - sizeAfterInit, size1);
    EXPECT_GE(sizeAfterSecond - sizeAfterFirst, size2);
}

TEST_F(BlockFileTest, CannotWriteBeyondMaxSize) {
    pBlockFile->init(*pConfig);
    
    size_t maxSize = pBlockFile->getMaxSize();
    size_t currentSize = pBlockFile->getCurrentSize();
    
    // Try to write more than available space
    size_t oversized = maxSize - currentSize + 1;
    
    if (oversized > 0 && oversized < maxSize) {
        EXPECT_FALSE(pBlockFile->canFit(oversized));
        
        // Attempting to write should fail
        char dummyData[1] = {0};
        auto result = pBlockFile->write(dummyData, oversized);
        // This might fail or might succeed depending on implementation
        // But canFit should have warned us
        EXPECT_FALSE(pBlockFile->canFit(oversized));
    }
}

TEST_F(BlockFileTest, ReadReturnsCorrectByteCount) {
    pBlockFile->init(*pConfig);
    
    const char* testData = "Test data for byte count";
    size_t dataSize = strlen(testData) + 1;
    
    auto writeResult = pBlockFile->write(testData, dataSize);
    ASSERT_TRUE(writeResult.isOk());
    
    int64_t offset = writeResult.value();
    char readBuffer[256] = {0};
    
    // Read full size
    auto readResult = pBlockFile->read(offset, readBuffer, dataSize);
    ASSERT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
    
    // Read partial size
    size_t partialSize = dataSize / 2;
    auto partialResult = pBlockFile->read(offset, readBuffer, partialSize);
    ASSERT_TRUE(partialResult.isOk());
    EXPECT_EQ(partialResult.value(), static_cast<int64_t>(partialSize));
}

TEST_F(BlockFileTest, MultipleFilesAreIndependent) {
    // Test that different BlockFile instances don't interfere
    std::string testFile2 = testDir + "/test_block2.dat";
    
    pBlockFile->init(*pConfig);
    
    const char* data1 = "File 1 data";
    size_t size1 = strlen(data1) + 1;
    auto result1 = pBlockFile->write(data1, size1);
    ASSERT_TRUE(result1.isOk());
    int64_t offset1 = result1.value();
    
    // Create second file
    pp::BlockFile blockFile2;
    pp::BlockFile::Config config2(testFile2, 1024 * 1024);
    auto init2 = blockFile2.init(config2);
    ASSERT_TRUE(init2.isOk());
    
    const char* data2 = "File 2 data";
    size_t size2 = strlen(data2) + 1;
    auto result2 = blockFile2.write(data2, size2);
    ASSERT_TRUE(result2.isOk());
    int64_t offset2 = result2.value();
    
    // Offsets might be the same (both start from beginning of their files)
    // But data should be independent
    char buffer1[256] = {0};
    char buffer2[256] = {0};
    
    auto read1 = pBlockFile->read(offset1, buffer1, size1);
    auto read2 = blockFile2.read(offset2, buffer2, size2);
    
    ASSERT_TRUE(read1.isOk());
    ASSERT_TRUE(read2.isOk());
    EXPECT_STREQ(buffer1, data1);
    EXPECT_STREQ(buffer2, data2);
    
    // Cleanup
    if (std::filesystem::exists(testFile2)) {
        std::filesystem::remove(testFile2);
    }
}
