#include "BlockFile.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <vector>

class BlockFileTest : public ::testing::Test {
protected:
  std::string testDir;
  std::string testFile;
  pp::BlockFile blockFile;
  pp::BlockFile::Config config;

  void SetUp() override {
    testDir = "/tmp/pp-ledger-test";
    testFile = testDir + "/test_block.dat";

    if (std::filesystem::exists(testFile)) {
      std::filesystem::remove(testFile);
    }
    if (!std::filesystem::exists(testDir)) {
      std::filesystem::create_directories(testDir);
    }

    // BlockFile requires at least 1MB max size
    config = pp::BlockFile::Config(testFile, 1024 * 1024); // 1MB
  }

  void TearDown() override {
    if (std::filesystem::exists(testFile)) {
      std::filesystem::remove(testFile);
    }
  }
};

TEST_F(BlockFileTest, InitializesSuccessfully) {
  auto result = blockFile.init(config);
  EXPECT_TRUE(result.isOk());
  EXPECT_TRUE(blockFile.isOpen());

  // After initialization, file should exist and have header size
  EXPECT_TRUE(std::filesystem::exists(testFile));
  EXPECT_GE(blockFile.getCurrentSize(), 0);
  EXPECT_LE(blockFile.getCurrentSize(), blockFile.getMaxSize());
}

TEST_F(BlockFileTest, WriteAndRead) {
  blockFile.init(config);

  const char *testData = "Hello, BlockFile!";
  size_t dataSize = strlen(testData) + 1;

  // Write data (returns block index)
  auto writeResult = blockFile.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());

  int64_t blockIndex = writeResult.value();
  EXPECT_EQ(blockIndex, 0); // First block should have index 0

  // Read data back using index-based read
  char readBuffer[256] = {0};
  auto readResult = blockFile.readBlock(0, readBuffer, sizeof(readBuffer));
  ASSERT_TRUE(readResult.isOk());
  EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
  EXPECT_STREQ(readBuffer, testData);
}

TEST_F(BlockFileTest, MultipleWrites) {
  blockFile.init(config);

  const char *data1 = "First block";
  const char *data2 = "Second block";
  size_t size1 = strlen(data1) + 1;
  size_t size2 = strlen(data2) + 1;

  auto result1 = blockFile.write(data1, size1);
  auto result2 = blockFile.write(data2, size2);

  ASSERT_TRUE(result1.isOk());
  ASSERT_TRUE(result2.isOk());

  // Write returns block index
  EXPECT_EQ(result1.value(), 0);
  EXPECT_EQ(result2.value(), 1);

  // Verify block count
  EXPECT_EQ(blockFile.getBlockCount(), 2);

  // Verify we can read both back using index-based read
  char readBuffer1[256] = {0};
  char readBuffer2[256] = {0};

  auto read1 = blockFile.readBlock(0, readBuffer1, sizeof(readBuffer1));
  auto read2 = blockFile.readBlock(1, readBuffer2, sizeof(readBuffer2));

  ASSERT_TRUE(read1.isOk());
  ASSERT_TRUE(read2.isOk());
  EXPECT_EQ(read1.value(), static_cast<int64_t>(size1));
  EXPECT_EQ(read2.value(), static_cast<int64_t>(size2));
  EXPECT_STREQ(readBuffer1, data1);
  EXPECT_STREQ(readBuffer2, data2);
}

TEST_F(BlockFileTest, CanFit) {
  blockFile.init(config);

  size_t maxSize = blockFile.getMaxSize();
  size_t currentSize = blockFile.getCurrentSize();
  // canFit accounts for size prefix overhead internally
  // Available space for data = maxSize - currentSize - SIZE_PREFIX_BYTES (8)
  size_t availableForData = maxSize - currentSize - 8;

  // Should be able to fit data that leaves room for size prefix
  EXPECT_TRUE(blockFile.canFit(availableForData));
  EXPECT_FALSE(blockFile.canFit(availableForData + 1));

  // Try to write data larger than max size
  size_t hugeSize = 1 * 1024 * 1024 * 1024; // 1GB (larger than max of 1MB)
  EXPECT_FALSE(blockFile.canFit(hugeSize));
}

TEST_F(BlockFileTest, FileSizeIncreasesWithWrites) {
  blockFile.init(config);

  size_t sizeAfterInit = blockFile.getCurrentSize();

  const char *data = "Test data";
  size_t dataSize = strlen(data) + 1;

  auto result = blockFile.write(data, dataSize);
  ASSERT_TRUE(result.isOk());

  size_t sizeAfterWrite = blockFile.getCurrentSize();
  EXPECT_GT(sizeAfterWrite, sizeAfterInit);
  EXPECT_GE(sizeAfterWrite - sizeAfterInit, dataSize);
}

TEST_F(BlockFileTest, ReopensPersistentFile) {
  // Write data to file
  blockFile.init(config);

  const char *testData = "Persistent data";
  size_t dataSize = strlen(testData) + 1;
  auto writeResult = blockFile.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());
  EXPECT_EQ(writeResult.value(), 0); // First block
  
  size_t fileSize = blockFile.getCurrentSize();
  uint64_t blockCount = blockFile.getBlockCount();
  EXPECT_EQ(blockCount, 1);

  // Close and reopen
  blockFile.close();
  pp::BlockFile blockFile2;
  auto reopenResult = blockFile2.init(config);
  ASSERT_TRUE(reopenResult.isOk());

  // File size should be restored
  EXPECT_EQ(blockFile2.getCurrentSize(), fileSize);
  
  // Block count should be restored from header
  EXPECT_EQ(blockFile2.getBlockCount(), blockCount);

  // Data should be readable using index-based read
  char readBuffer[256] = {0};
  auto readResult = blockFile2.readBlock(0, readBuffer, sizeof(readBuffer));
  ASSERT_TRUE(readResult.isOk());
  EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
  EXPECT_STREQ(readBuffer, testData);
}

TEST_F(BlockFileTest, FileSizeMatchesActualFileSize) {
  blockFile.init(config);

  // After init, reported size should match actual file size
  size_t reportedSize = blockFile.getCurrentSize();
  size_t actualFileSize = std::filesystem::file_size(testFile);
  EXPECT_EQ(reportedSize, actualFileSize);

  // After writing, sizes should still match
  const char *testData = "Test data";
  size_t dataSize = strlen(testData) + 1;
  auto writeResult = blockFile.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());

  reportedSize = blockFile.getCurrentSize();
  actualFileSize = std::filesystem::file_size(testFile);
  EXPECT_EQ(reportedSize, actualFileSize);
}

TEST_F(BlockFileTest, ReadBlockReturnsCorrectByteCount) {
  blockFile.init(config);

  const char *testData = "Test data for byte count";
  size_t dataSize = strlen(testData) + 1;

  auto writeResult = blockFile.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());
  EXPECT_EQ(writeResult.value(), 0); // First block

  char readBuffer[256] = {0};

  // Read block and verify byte count
  auto readResult = blockFile.readBlock(0, readBuffer, sizeof(readBuffer));
  ASSERT_TRUE(readResult.isOk());
  EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
  EXPECT_STREQ(readBuffer, testData);

  // Verify getBlockSize returns correct size
  auto sizeResult = blockFile.getBlockSize(0);
  ASSERT_TRUE(sizeResult.isOk());
  EXPECT_EQ(sizeResult.value(), dataSize);
}

TEST_F(BlockFileTest, MultipleFilesAreIndependent) {
  // Test that different BlockFile instances don't interfere
  std::string testFile2 = testDir + "/test_block2.dat";

  blockFile.init(config);

  const char *data1 = "File 1 data";
  size_t size1 = strlen(data1) + 1;
  auto result1 = blockFile.write(data1, size1);
  ASSERT_TRUE(result1.isOk());
  EXPECT_EQ(result1.value(), 0); // First block in file 1

  // Create second file
  pp::BlockFile blockFile2;
  pp::BlockFile::Config config2(testFile2, 1024 * 1024);
  auto init2 = blockFile2.init(config2);
  ASSERT_TRUE(init2.isOk());

  const char *data2 = "File 2 data";
  size_t size2 = strlen(data2) + 1;
  auto result2 = blockFile2.write(data2, size2);
  ASSERT_TRUE(result2.isOk());
  EXPECT_EQ(result2.value(), 0); // First block in file 2

  // Each file should have 1 block
  EXPECT_EQ(blockFile.getBlockCount(), 1);
  EXPECT_EQ(blockFile2.getBlockCount(), 1);

  // Data should be independent (use index-based read)
  char buffer1[256] = {0};
  char buffer2[256] = {0};

  auto read1 = blockFile.readBlock(0, buffer1, sizeof(buffer1));
  auto read2 = blockFile2.readBlock(0, buffer2, sizeof(buffer2));

  ASSERT_TRUE(read1.isOk());
  ASSERT_TRUE(read2.isOk());
  EXPECT_EQ(read1.value(), static_cast<int64_t>(size1));
  EXPECT_EQ(read2.value(), static_cast<int64_t>(size2));
  EXPECT_STREQ(buffer1, data1);
  EXPECT_STREQ(buffer2, data2);

  // Cleanup
  if (std::filesystem::exists(testFile2)) {
    std::filesystem::remove(testFile2);
  }
}

TEST_F(BlockFileTest, CannotWriteBeyondMaxSize) {
  blockFile.init(config);

  size_t maxSize = blockFile.getMaxSize();
  size_t currentSize = blockFile.getCurrentSize();

  // Try to write more than available space
  size_t oversized = maxSize - currentSize + 1;

  if (oversized > 0 && oversized < maxSize) {
    EXPECT_FALSE(blockFile.canFit(oversized));

    // Attempting to write should fail
    std::vector<char> dummyData(oversized, 0);
    auto result = blockFile.write(dummyData.data(), oversized);
    EXPECT_FALSE(result.isOk());
  }
}

TEST_F(BlockFileTest, RequiresMinimumMaxSize) {
  // Test that init fails with max size less than 1MB
  pp::BlockFile::Config smallConfig(testFile, 512 * 1024); // 512KB (too small)
  auto result = blockFile.init(smallConfig);
  EXPECT_FALSE(result.isOk());
}
