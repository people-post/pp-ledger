#include "ledger/ledger/FileStore.h"
#include "testing/TestTempPath.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

class FileStoreTest : public ::testing::Test {
protected:
  std::string testDir;
  std::string testFile;
  pp::FileStore fileStore;
  pp::FileStore::InitConfig config;

  void SetUp() override {
    testDir = pp::test::uniqueTempPath("pp-ledger-test").string();
    testFile = testDir + "/test_block.dat";

    std::error_code ec;
    if (std::filesystem::exists(testFile, ec)) {
      std::filesystem::remove(testFile, ec);
    }
    if (!std::filesystem::exists(testDir, ec)) {
      std::filesystem::create_directories(testDir, ec);
    }

    // FileStore requires at least 1MB max size
    config.filepath = testFile;
    config.maxSize = 1024 * 1024; // 1MB
  }

  void TearDown() override {
    // Close before delete — Windows cannot remove an open file handle.
    fileStore.close();
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
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
  pp::FileStore::InitConfig config2;
  config2.filepath = testFile2;
  config2.maxSize = 1024 * 1024;
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

  // Cleanup (close before remove — required on Windows)
  fileStore2.close();
  std::error_code ec;
  std::filesystem::remove(testFile2, ec);
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

// Regression: a corrupted (or maliciously crafted) block-size prefix near
// UINT64_MAX used to overflow the `offset + size` bounds check and could
// leave buildBlockIndex() looping without making progress. The scan must
// bail out cleanly and getBlockCount()/readBlock() must stay usable.
TEST_F(FileStoreTest, BuildBlockIndexRejectsCorruptedHugeBlockSize) {
  fileStore.init(config);

  const char *testData = "valid block";
  size_t dataSize = strlen(testData) + 1;
  ASSERT_TRUE(fileStore.write(testData, dataSize).isOk());
  fileStore.close();

  // Append a corrupted block: a size prefix that claims far more data than
  // remains in the file, followed by a little garbage.
  {
    std::fstream raw(testFile, std::ios::binary | std::ios::in | std::ios::out |
                                    std::ios::ate);
    ASSERT_TRUE(raw.is_open());
    uint64_t hugeSize = UINT64_MAX - 3;
    raw.write(reinterpret_cast<const char *>(&hugeSize), sizeof(hugeSize));
    raw.write("junk", 4);
  }

  pp::FileStore fileStore2;
  auto mountResult = fileStore2.mount(testFile, 1024 * 1024);
  ASSERT_TRUE(mountResult.isOk());

  // Reading must complete (not hang) and must not see the corrupted second
  // "block" as valid data.
  char readBuffer[256] = {0};
  auto readResult0 = fileStore2.readBlock(0, readBuffer, sizeof(readBuffer));
  ASSERT_TRUE(readResult0.isOk());
  EXPECT_STREQ(readBuffer, testData);

  auto readResult1 = fileStore2.readBlock(1, readBuffer, sizeof(readBuffer));
  EXPECT_FALSE(readResult1.isOk());
}

TEST_F(FileStoreTest, RequiresMinimumMaxSize) {
  // Test that init fails with max size less than 1MB
  pp::FileStore::InitConfig smallConfig;
  smallConfig.filepath = testFile;
  smallConfig.maxSize = 512 * 1024; // 512KB (too small)
  auto result = fileStore.init(smallConfig);
  EXPECT_FALSE(result.isOk());
}

// --- Commit point: the header's block count (durable writes) ---

namespace {

/** The file as a power loss would leave it right now: what reached the file, nothing more. */
std::string snapshot(const std::string &file, const std::string &name) {
  const std::string copy = std::filesystem::path(file).parent_path().string() + "/" + name;
  std::filesystem::copy_file(file, copy, std::filesystem::copy_options::overwrite_existing);
  return copy;
}

} // namespace

// Appends write data; only commit() writes the count that makes them survive a restart. Mount keeps
// the committed blocks and drops the uncommitted tail, so the next append does not land after it.
TEST_F(FileStoreTest, MountKeepsOnlyCommittedBlocks) {
  ASSERT_TRUE(fileStore.init(config).isOk());
  ASSERT_TRUE(fileStore.appendBlock("block-0").isOk());
  ASSERT_TRUE(fileStore.appendBlock("block-1").isOk());
  ASSERT_TRUE(fileStore.commit().isOk());
  EXPECT_FALSE(fileStore.hasUncommitted());
  const auto committedSize = fileStore.getCurrentSize();
  ASSERT_TRUE(fileStore.appendBlock("block-2 never committed").isOk());
  EXPECT_TRUE(fileStore.hasUncommitted());

  const std::string crashed = snapshot(testFile, "crashed.dat");
  pp::FileStore restarted;
  ASSERT_TRUE(restarted.mount(crashed, config.maxSize).isOk());
  EXPECT_EQ(restarted.getBlockCount(), 2u);
  EXPECT_EQ(std::filesystem::file_size(crashed), committedSize) << "the uncommitted tail is gone";
  ASSERT_TRUE(restarted.appendBlock("block-2 again").isOk());
  EXPECT_EQ(restarted.readBlock(2).value(), "block-2 again");
  EXPECT_EQ(restarted.readBlock(1).value(), "block-1");
  restarted.close();
}

// A torn write after the committed blocks (a power loss mid-append) is dropped, however it looks.
TEST_F(FileStoreTest, MountDropsATornTail) {
  ASSERT_TRUE(fileStore.init(config).isOk());
  ASSERT_TRUE(fileStore.appendBlock("block-0").isOk());
  ASSERT_TRUE(fileStore.commit().isOk());
  fileStore.close();
  const auto committedSize = std::filesystem::file_size(testFile);
  {
    std::ofstream out(testFile, std::ios::binary | std::ios::app);
    out << std::string(100, '\0'); // zeros parse as size prefixes of empty blocks
  }
  pp::FileStore restarted;
  ASSERT_TRUE(restarted.mount(testFile, config.maxSize).isOk());
  EXPECT_EQ(restarted.getBlockCount(), 1u);
  EXPECT_EQ(std::filesystem::file_size(testFile), committedSize);
  restarted.close();
}

// A header committing more blocks than the file holds is refused, not silently shortened.
TEST_F(FileStoreTest, MountRefusesAHeaderAboveItsData) {
  ASSERT_TRUE(fileStore.init(config).isOk());
  ASSERT_TRUE(fileStore.appendBlock("block-0").isOk());
  ASSERT_TRUE(fileStore.appendBlock("block-1").isOk());
  ASSERT_TRUE(fileStore.commit().isOk());
  fileStore.close();
  std::filesystem::resize_file(testFile, std::filesystem::file_size(testFile) - 3);
  pp::FileStore restarted;
  auto mounted = restarted.mount(testFile, config.maxSize);
  ASSERT_FALSE(mounted.isOk());
  EXPECT_NE(mounted.error().message.find("header commits 2"), std::string::npos) << mounted.error().message;
}

// Rewind lowers the commit point and the data together; the result mounts as rewound.
TEST_F(FileStoreTest, RewindIsCommitted) {
  ASSERT_TRUE(fileStore.init(config).isOk());
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(fileStore.appendBlock("block-" + std::to_string(i)).isOk());
  }
  ASSERT_TRUE(fileStore.rewindTo(1).isOk());
  EXPECT_FALSE(fileStore.hasUncommitted());
  pp::FileStore restarted;
  ASSERT_TRUE(restarted.mount(snapshot(testFile, "rewound.dat"), config.maxSize).isOk());
  EXPECT_EQ(restarted.getBlockCount(), 1u);
  EXPECT_EQ(restarted.readBlock(0).value(), "block-0");
  restarted.close();
}
