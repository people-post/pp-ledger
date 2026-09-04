#include "VolumeStore.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

class VolumeStoreTest : public ::testing::Test {
protected:
  pp::VolumeStore store;
  pp::VolumeStore::InitConfig config;
  std::string testDir;

  void SetUp() override {
    store.redirectLogger("volumestore");
    testDir = (std::filesystem::temp_directory_path() /
               "pp-ledger-volumestore-test")
                  .string();
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
  for (const auto &b : blocks) {
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
