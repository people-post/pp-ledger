#include "ledger/ledger/VolumeStore.h"
#include "testing/TestTempPath.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

class VolumeStoreTest : public ::testing::Test {
protected:
  pp::VolumeStore store;
  pp::VolumeStore::InitConfig config;
  std::string testDir;

  void SetUp() override {
    store.redirectLogger("volumestore");
    testDir = pp::test::uniqueTempPath("pp-ledger-volumestore-test").string();
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);

    config = pp::VolumeStore::InitConfig();
    config.volumesDir = testDir;
    config.maxFileCount = 2;
    config.maxFileSize = 1024 * 1024; // 1MB
    config.maxVolumes = 4;
  }

  void TearDown() override {
    store.close();
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
  }
};

TEST_F(VolumeStoreTest, InitializesWithFirstVolume) {
  auto result = store.init(config);
  ASSERT_TRUE(result.isOk()) << result.error().message;
  EXPECT_EQ(store.getBlockCount(), 0u);
  EXPECT_EQ(store.getVolumeCount(), 1u);
  EXPECT_TRUE(std::filesystem::exists(testDir + "/volumes_idx.dat"));
  EXPECT_TRUE(std::filesystem::exists(testDir + "/v000001"));
}

TEST_F(VolumeStoreTest, FailsWithInvalidConfig) {
  config.maxVolumes = 0;
  EXPECT_TRUE(store.init(config).isError());

  config.maxVolumes = 4;
  config.maxFileCount = 0;
  EXPECT_TRUE(store.init(config).isError());

  config.maxFileCount = 2;
  config.maxFileSize = 100;
  EXPECT_TRUE(store.init(config).isError());
}

TEST_F(VolumeStoreTest, WritesAndReadsBlocks) {
  ASSERT_TRUE(store.init(config).isOk());

  std::string data = "hello-block";
  auto append = store.appendBlock(data);
  ASSERT_TRUE(append.isOk()) << append.error().message;
  EXPECT_EQ(append.value(), 0u);

  auto read = store.readBlock(0);
  ASSERT_TRUE(read.isOk()) << read.error().message;
  EXPECT_EQ(read.value(), data);
  EXPECT_EQ(store.getBlockCount(), 1u);
}

TEST_F(VolumeStoreTest, PersistsAcrossRemount) {
  ASSERT_TRUE(store.init(config).isOk());
  std::vector<std::string> blocks = {"a", "b", "c"};
  for (const auto& b : blocks) {
    ASSERT_TRUE(store.appendBlock(b).isOk());
  }
  store.close();

  pp::VolumeStore store2;
  store2.redirectLogger("volumestore2");
  pp::VolumeStore::MountConfig mountConfig;
  mountConfig.volumesDir = testDir;
  ASSERT_TRUE(store2.mount(mountConfig).isOk());
  EXPECT_EQ(store2.getBlockCount(), blocks.size());
  for (size_t i = 0; i < blocks.size(); ++i) {
    auto read = store2.readBlock(i);
    ASSERT_TRUE(read.isOk()) << read.error().message;
    EXPECT_EQ(read.value(), blocks[i]);
  }
}

TEST_F(VolumeStoreTest, RollsOverToSecondVolume) {
  // Tiny volume: 1 file, ~1MB — fill with large blocks then force new volume
  config.maxFileCount = 1;
  config.maxVolumes = 3;
  ASSERT_TRUE(store.init(config).isOk());
  EXPECT_EQ(store.getVolumeCount(), 1u);

  std::string large(200 * 1024, 'X'); // 200KB
  size_t added = 0;
  for (size_t i = 0; i < 20; ++i) {
    if (!store.canFit(large.size())) {
      break;
    }
    auto result = store.appendBlock(large + std::to_string(i));
    if (!result.isOk()) {
      break;
    }
    added++;
  }
  ASSERT_GT(added, 0u);

  // Keep writing until a second volume appears (or we hit max)
  while (store.getVolumeCount() < 2 && store.canFit(large.size())) {
    auto result = store.appendBlock(large + std::to_string(added));
    ASSERT_TRUE(result.isOk()) << result.error().message;
    added++;
  }

  EXPECT_GE(store.getVolumeCount(), 2u);
  EXPECT_TRUE(std::filesystem::exists(testDir + "/v000002"));

  // Full readback
  for (size_t i = 0; i < store.getBlockCount(); ++i) {
    auto read = store.readBlock(i);
    ASSERT_TRUE(read.isOk()) << "block " << i << ": " << read.error().message;
  }
}

TEST_F(VolumeStoreTest, CanFitFalseAtMaxVolumes) {
  config.maxFileCount = 1;
  config.maxVolumes = 2;
  ASSERT_TRUE(store.init(config).isOk());

  std::string large(200 * 1024, 'Y');
  while (store.canFit(large.size())) {
    auto result = store.appendBlock(large);
    ASSERT_TRUE(result.isOk()) << result.error().message;
  }

  EXPECT_EQ(store.getVolumeCount(), 2u);
  EXPECT_FALSE(store.canFit(large.size()));
  EXPECT_TRUE(store.appendBlock(large).isError());
}

TEST_F(VolumeStoreTest, RewindAcrossVolumes) {
  config.maxFileCount = 1;
  config.maxVolumes = 4;
  ASSERT_TRUE(store.init(config).isOk());

  std::string large(200 * 1024, 'Z');
  while (store.getVolumeCount() < 2) {
    ASSERT_TRUE(store.canFit(large.size()));
    ASSERT_TRUE(store.appendBlock(large).isOk());
  }
  uint64_t countBefore = store.getBlockCount();
  ASSERT_GT(countBefore, 1u);

  // Rewind to keep only first block
  ASSERT_TRUE(store.rewindTo(1).isOk());
  EXPECT_EQ(store.getBlockCount(), 1u);

  auto read = store.readBlock(0);
  ASSERT_TRUE(read.isOk());
  EXPECT_EQ(read.value(), large);
}

TEST_F(VolumeStoreTest, CanFitRejectsOversizedBlock) {
  ASSERT_TRUE(store.init(config).isOk());
  EXPECT_FALSE(store.canFit(config.maxFileSize + 1));
}

// --- A crash just after a new volume (review H1) ---

namespace {

/** Index entries are packed big-endian; these read and write the u64 that ends a buffer. */
uint64_t readBigEndianTail(const std::string& bytes) {
  uint64_t value = 0;
  for (size_t i = bytes.size() - sizeof(uint64_t); i < bytes.size(); ++i) {
    value = (value << 8) | static_cast<unsigned char>(bytes[i]);
  }
  return value;
}

void writeBigEndianTail(std::string& bytes, uint64_t value) {
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[bytes.size() - 1 - i] = static_cast<char>((value >> (8 * i)) & 0xff);
  }
}

std::string snapshotDir(const std::string& dir, const std::string& suffix) {
  const std::string copy = dir + suffix;
  std::error_code ec;
  std::filesystem::remove_all(copy, ec);
  std::filesystem::copy(dir, copy, std::filesystem::copy_options::recursive);
  return copy;
}

std::string bigBlock(uint64_t index) {
  return std::string(300 * 1024, static_cast<char>('a' + index % 26)) + std::to_string(index);
}

uint64_t lastStartId(const std::string& indexPath) {
  std::ifstream in(indexPath, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return bytes.size() >= sizeof(uint64_t) ? readBigEndianTail(bytes) : 0;
}

void setLastStartId(const std::string& indexPath, uint64_t to) {
  std::string bytes;
  {
    std::ifstream in(indexPath, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  writeBigEndianTail(bytes, to);
  std::ofstream out(indexPath, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

// A new volume is numbered from the blocks before it; a crash must not leave it numbered past
// blocks of the old volume that were never committed. So opening a volume commits the one it leaves.
TEST_F(VolumeStoreTest, ACrashAfterANewVolumeKeepsBlockIdsContiguous) {
  ASSERT_TRUE(store.init(config).isOk());
  ASSERT_TRUE(store.appendBlock(bigBlock(0)).isOk());
  ASSERT_TRUE(store.commit().isOk());
  const size_t volumesBefore = store.getVolumeCount();
  uint64_t count = 1;
  while (store.getVolumeCount() == volumesBefore) {
    ASSERT_LT(count, 40u) << "no new volume";
    ASSERT_TRUE(store.appendBlock(bigBlock(count)).isOk());
    ++count;
  }
  ASSERT_GE(count, 3u) << "blocks 1.." << count - 2 << " sat uncommitted in the old volume";

  const std::string crashed = snapshotDir(testDir, "-crashed");
  {
    pp::VolumeStore restarted;
    restarted.redirectLogger("volumestore2");
    ASSERT_TRUE(restarted.mount({crashed}).isOk());
    for (uint64_t i = restarted.getBlockCount(); i < count; ++i) { // what a node syncs again
      auto appended = restarted.appendBlock(bigBlock(i));
      ASSERT_TRUE(appended.isOk()) << appended.error().message;
      EXPECT_EQ(appended.value(), i);
    }
    for (uint64_t i = 0; i < count; ++i) {
      auto read = restarted.readBlock(i);
      ASSERT_TRUE(read.isOk()) << "block " << i << ": " << read.error().message;
      EXPECT_EQ(read.value(), bigBlock(i)) << "block " << i;
    }
    restarted.close();
  }
  std::error_code ec;
  std::filesystem::remove_all(crashed, ec);
}

// A build without that commit could already have listed a volume numbered past the end of the one
// before it; a mount numbers each volume from the blocks before it, and saves that.
TEST_F(VolumeStoreTest, AMountNumbersEachVolumeFromTheBlocksBeforeIt) {
  ASSERT_TRUE(store.init(config).isOk());
  uint64_t count = 0;
  while (store.getVolumeCount() < 2 || count < 8) {
    ASSERT_LT(count, 40u);
    ASSERT_TRUE(store.appendBlock(bigBlock(count)).isOk());
    ++count;
  }
  store.close();
  const std::string indexPath = testDir + "/volumes_idx.dat";
  const uint64_t start = lastStartId(indexPath);
  ASSERT_GT(start, 0u) << "the last volume does not start at block 0";
  setLastStartId(indexPath, start + 5); // as the old rollover left it after a crash

  {
    pp::VolumeStore remounted;
    remounted.redirectLogger("volumestore2");
    ASSERT_TRUE(remounted.mount({testDir}).isOk());
    EXPECT_EQ(remounted.getBlockCount(), count);
    for (uint64_t i = 0; i < count; ++i) {
      auto read = remounted.readBlock(i);
      ASSERT_TRUE(read.isOk()) << "block " << i << ": " << read.error().message;
      EXPECT_EQ(read.value(), bigBlock(i)) << "block " << i;
    }
    remounted.close();
  }
  EXPECT_EQ(lastStartId(indexPath), start) << "the repaired index is saved";
}
