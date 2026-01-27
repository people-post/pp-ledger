#include "FileStore.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <vector>

class FileStoreTest : public ::testing::Test {
protected:
  std::string testDir;
  std::string testFile;
  pp::FileStore fileStore;
  pp::FileStore::InitConfig config;

  void SetUp() override {
    testDir = "/tmp/pp-ledger-test";
    testFile = testDir + "/test_block.dat";

    if (std::filesystem::exists(testFile)) {
      std::filesystem::remove(testFile);
    }
    if (!std::filesystem::exists(testDir)) {
      std::filesystem::create_directories(testDir);
    }

    // FileStore requires at least 1MB max size
    config = pp::FileStore::InitConfig(testFile, 1024 * 1024); // 1MB
  }

  void TearDown() override {
    if (std::filesystem::exists(testFile)) {
      std::filesystem::remove(testFile);
    }
  }
};

TEST_F(FileStoreTest, InitializesSuccessfully) {
  auto result = fileStore.init(config);
  EXPECT_TRUE(result.isOk());
  EXPECT_TRUE(fileStore.isOpen());

  // After initialization, file should exist and have header size
  EXPECT_TRUE(std::filesystem::exists(testFile));
  EXPECT_GE(fileStore.getCurrentSize(), 0);
  EXPECT_LE(fileStore.getCurrentSize(), fileStore.getMaxSize());
}

TEST_F(FileStoreTest, WriteAndRead) {
  fileStore.init(config);

  const char *testData = "Hello, FileStore!";
  size_t dataSize = strlen(testData) + 1;

  // Write data (returns block index)
  auto writeResult = fileStore.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());

  int64_t blockIndex = writeResult.value();
  EXPECT_EQ(blockIndex, 0); // First block should have index 0

  // Read data back using index-based read
  char readBuffer[256] = {0};
  auto readResult = fileStore.readBlock(0, readBuffer, sizeof(readBuffer));
  ASSERT_TRUE(readResult.isOk());
  EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
  EXPECT_STREQ(readBuffer, testData);
}

TEST_F(FileStoreTest, MultipleWrites) {
  fileStore.init(config);

  const char *data1 = "First block";
  const char *data2 = "Second block";
  size_t size1 = strlen(data1) + 1;
  size_t size2 = strlen(data2) + 1;

  auto result1 = fileStore.write(data1, size1);
  auto result2 = fileStore.write(data2, size2);

  ASSERT_TRUE(result1.isOk());
  ASSERT_TRUE(result2.isOk());

  // Write returns block index
  EXPECT_EQ(result1.value(), 0);
  EXPECT_EQ(result2.value(), 1);

  // Verify block count
  EXPECT_EQ(fileStore.getBlockCount(), 2);

  // Verify we can read both back using index-based read
  char readBuffer1[256] = {0};
  char readBuffer2[256] = {0};

  auto read1 = fileStore.readBlock(0, readBuffer1, sizeof(readBuffer1));
  auto read2 = fileStore.readBlock(1, readBuffer2, sizeof(readBuffer2));

  ASSERT_TRUE(read1.isOk());
  ASSERT_TRUE(read2.isOk());
  EXPECT_EQ(read1.value(), static_cast<int64_t>(size1));
  EXPECT_EQ(read2.value(), static_cast<int64_t>(size2));
  EXPECT_STREQ(readBuffer1, data1);
  EXPECT_STREQ(readBuffer2, data2);
}

TEST_F(FileStoreTest, CanFit) {
  fileStore.init(config);

  size_t maxSize = fileStore.getMaxSize();
  size_t currentSize = fileStore.getCurrentSize();
  // canFit accounts for size prefix overhead internally
  // Available space for data = maxSize - currentSize - SIZE_PREFIX_BYTES (8)
  size_t availableForData = maxSize - currentSize - 8;

  // Should be able to fit data that leaves room for size prefix
  EXPECT_TRUE(fileStore.canFit(availableForData));
  EXPECT_FALSE(fileStore.canFit(availableForData + 1));

  // Try to write data larger than max size
  size_t hugeSize = 1 * 1024 * 1024 * 1024; // 1GB (larger than max of 1MB)
  EXPECT_FALSE(fileStore.canFit(hugeSize));
}

TEST_F(FileStoreTest, FileSizeIncreasesWithWrites) {
  fileStore.init(config);

  size_t sizeAfterInit = fileStore.getCurrentSize();

  const char *data = "Test data";
  size_t dataSize = strlen(data) + 1;

  auto result = fileStore.write(data, dataSize);
  ASSERT_TRUE(result.isOk());

  size_t sizeAfterWrite = fileStore.getCurrentSize();
  EXPECT_GT(sizeAfterWrite, sizeAfterInit);
  EXPECT_GE(sizeAfterWrite - sizeAfterInit, dataSize);
}

TEST_F(FileStoreTest, ReopensPersistentFile) {
  // Write data to file
  fileStore.init(config);

  const char *testData = "Persistent data";
  size_t dataSize = strlen(testData) + 1;
  auto writeResult = fileStore.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());
  EXPECT_EQ(writeResult.value(), 0); // First block
  
  size_t fileSize = fileStore.getCurrentSize();
  uint64_t blockCount = fileStore.getBlockCount();
  EXPECT_EQ(blockCount, 1);

  // Close and reopen
  fileStore.close();
  pp::FileStore fileStore2;
  auto reopenResult = fileStore2.mount(testFile, 1024 * 1024);
  ASSERT_TRUE(reopenResult.isOk());

  // File size should be restored
  EXPECT_EQ(fileStore2.getCurrentSize(), fileSize);
  
  // Block count should be restored from header
  EXPECT_EQ(fileStore2.getBlockCount(), blockCount);

  // Data should be readable using index-based read
  char readBuffer[256] = {0};
  auto readResult = fileStore2.readBlock(0, readBuffer, sizeof(readBuffer));
  ASSERT_TRUE(readResult.isOk());
  EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
  EXPECT_STREQ(readBuffer, testData);
}

TEST_F(FileStoreTest, FileSizeMatchesActualFileSize) {
  fileStore.init(config);

  // After init, reported size should match actual file size
  size_t reportedSize = fileStore.getCurrentSize();
  size_t actualFileSize = std::filesystem::file_size(testFile);
  EXPECT_EQ(reportedSize, actualFileSize);

  // After writing, sizes should still match
  const char *testData = "Test data";
  size_t dataSize = strlen(testData) + 1;
  auto writeResult = fileStore.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());

  reportedSize = fileStore.getCurrentSize();
  actualFileSize = std::filesystem::file_size(testFile);
  EXPECT_EQ(reportedSize, actualFileSize);
}

TEST_F(FileStoreTest, ReadBlockReturnsCorrectByteCount) {
  fileStore.init(config);

  const char *testData = "Test data for byte count";
  size_t dataSize = strlen(testData) + 1;

  auto writeResult = fileStore.write(testData, dataSize);
  ASSERT_TRUE(writeResult.isOk());
  EXPECT_EQ(writeResult.value(), 0); // First block

  char readBuffer[256] = {0};

  // Read block and verify byte count
  auto readResult = fileStore.readBlock(0, readBuffer, sizeof(readBuffer));
  ASSERT_TRUE(readResult.isOk());
  EXPECT_EQ(readResult.value(), static_cast<int64_t>(dataSize));
  EXPECT_STREQ(readBuffer, testData);

  // Verify getBlockSize returns correct size
  auto sizeResult = fileStore.getBlockSize(0);
  ASSERT_TRUE(sizeResult.isOk());
  EXPECT_EQ(sizeResult.value(), dataSize);
}

TEST_F(FileStoreTest, MultipleFilesAreIndependent) {
  // Test that different FileStore instances don't interfere
  std::string testFile2 = testDir + "/test_block2.dat";

  fileStore.init(config);

  const char *data1 = "File 1 data";
  size_t size1 = strlen(data1) + 1;
  auto result1 = fileStore.write(data1, size1);
  ASSERT_TRUE(result1.isOk());
  EXPECT_EQ(result1.value(), 0); // First block in file 1

  // Create second file
  pp::FileStore fileStore2;
  pp::FileStore::InitConfig config2(testFile2, 1024 * 1024);
  auto init2 = fileStore2.init(config2);
  ASSERT_TRUE(init2.isOk());

  const char *data2 = "File 2 data";
  size_t size2 = strlen(data2) + 1;
  auto result2 = fileStore2.write(data2, size2);
  ASSERT_TRUE(result2.isOk());
  EXPECT_EQ(result2.value(), 0); // First block in file 2

  // Each file should have 1 block
  EXPECT_EQ(fileStore.getBlockCount(), 1);
  EXPECT_EQ(fileStore2.getBlockCount(), 1);

  // Data should be independent (use index-based read)
  char buffer1[256] = {0};
  char buffer2[256] = {0};

  auto read1 = fileStore.readBlock(0, buffer1, sizeof(buffer1));
  auto read2 = fileStore2.readBlock(0, buffer2, sizeof(buffer2));

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

TEST_F(FileStoreTest, CannotWriteBeyondMaxSize) {
  fileStore.init(config);

  size_t maxSize = fileStore.getMaxSize();
  size_t currentSize = fileStore.getCurrentSize();

  // Try to write more than available space
  size_t oversized = maxSize - currentSize + 1;

  if (oversized > 0 && oversized < maxSize) {
    EXPECT_FALSE(fileStore.canFit(oversized));

    // Attempting to write should fail
    std::vector<char> dummyData(oversized, 0);
    auto result = fileStore.write(dummyData.data(), oversized);
    EXPECT_FALSE(result.isOk());
  }
}

TEST_F(FileStoreTest, RequiresMinimumMaxSize) {
  // Test that init fails with max size less than 1MB
  pp::FileStore::InitConfig smallConfig(testFile, 512 * 1024); // 512KB (too small)
  auto result = fileStore.init(smallConfig);
  EXPECT_FALSE(result.isOk());
}
