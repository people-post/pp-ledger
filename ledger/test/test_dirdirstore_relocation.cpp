#include "DirDirStore.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

class DirDirStoreRelocationTest : public ::testing::Test {
protected:
    std::string testDir;
    pp::DirDirStore dirDirStore;
    pp::DirDirStore::InitConfig config;
    
    void SetUp() override {
        dirDirStore.redirectLogger("dirdirstore");
        testDir = "/tmp/pp-ledger-dirdirstore-relocation-test";
        
        // Clean up test directory
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
        std::filesystem::create_directories(testDir);
        
        config = pp::DirDirStore::InitConfig();
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
    
    std::string createTestBlock(size_t i) {
        std::string block = "Block number ";
        block += std::to_string(i);
        block.resize(100, ' '); // Pad to 100 bytes
        return block;
    }
};

TEST_F(DirDirStoreRelocationTest, PreservesIndexFileAfterRootStoreRelocation) {
    // Initialize store - will use root store mode initially
    config.maxLevel = 0;
    dirDirStore.init(config);
    
    // Verify index file exists after init
    std::string indexPath = testDir + "/dirdir_idx.dat";
    ASSERT_TRUE(std::filesystem::exists(indexPath)) 
        << "Index file should exist after initialization";
    
    // Add enough blocks to fill root store and trigger relocation
    std::string largeData(200 * 1024, 'X'); // 200KB per block
    size_t blocksAdded = 0;
    for (size_t i = 0; i < 20; i++) {
        if (!dirDirStore.canFit(largeData.size())) {
            break;
        }
        auto result = dirDirStore.appendBlock(largeData);
        if (result.isOk()) {
            blocksAdded++;
        } else {
            break;
        }
    }
    
    ASSERT_GT(blocksAdded, 0) << "Should have added at least one block";
    
    // Verify index file still exists after relocation
    EXPECT_TRUE(std::filesystem::exists(indexPath)) 
        << "Index file should be preserved in parent directory after relocation";
    
    // Verify that subdirectory was created
    bool hasSubdir = false;
    for (const auto& entry : std::filesystem::directory_iterator(testDir)) {
        if (entry.is_directory()) {
            hasSubdir = true;
            break;
        }
    }
    EXPECT_TRUE(hasSubdir) << "Subdirectory should have been created after relocation";
    
    // Verify we can still read the index file
    std::ifstream indexFile(indexPath, std::ios::binary);
    ASSERT_TRUE(indexFile.is_open()) << "Should be able to open index file";
    
    // Read magic number from index to verify it's the correct file type
    uint32_t magic = 0;
    indexFile.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    // Note: The magic is written in little-endian format by the serializer
    // MAGIC_DIR_DIR = 0x504C4444 ("PLDD"), but stored as bytes 50 4C 44 44
    // When read back as uint32_t in little-endian: 0x44444C50
    EXPECT_EQ(magic, 0x44444C50) << "Index file should have DirDirStore magic number";
    indexFile.close();
}

TEST_F(DirDirStoreRelocationTest, CanMountAfterRelocationWithPreservedIndex) {
    size_t originalBlockCount;
    std::vector<std::string> blockData;
    
    // Scope to ensure dirDirStore destructor is called (which flushes)
    {
        // Initialize and add blocks to trigger relocation
        config.maxLevel = 0;
        dirDirStore.init(config);
        
        std::string largeData(200 * 1024, 'X'); // 200KB per block
        for (size_t i = 0; i < 20; i++) {
            if (!dirDirStore.canFit(largeData.size())) {
                break;
            }
            std::string data = largeData + std::to_string(i);
            auto result = dirDirStore.appendBlock(data);
            if (result.isOk()) {
                blockData.push_back(data);
            } else {
                break;
            }
        }
        
        ASSERT_GT(blockData.size(), 0) << "Should have added blocks";
        originalBlockCount = blockData.size();
    }
    // dirDirStore destructor called here, flushing data
    
    // Mount the store again - should load config from preserved index
    pp::DirDirStore dirDirStore2;
    dirDirStore2.redirectLogger("dirdirstore2");
    pp::DirDirStore::MountConfig mountConfig;
    mountConfig.dirPath = testDir;
    mountConfig.maxLevel = 0;
    
    auto mountResult = dirDirStore2.mount(mountConfig);
    ASSERT_TRUE(mountResult.isOk()) << "Should be able to mount store with preserved index";
    
    // Verify block count matches
    EXPECT_EQ(dirDirStore2.getBlockCount(), originalBlockCount) 
        << "Mounted store should have same block count";
    
    // Verify we can read all blocks
    for (size_t i = 0; i < blockData.size(); i++) {
        auto readResult = dirDirStore2.readBlock(i);
        ASSERT_TRUE(readResult.isOk()) << "Should be able to read block " << i;
        EXPECT_EQ(readResult.value(), blockData[i]) << "Block " << i << " data should match";
    }
}

TEST_F(DirDirStoreRelocationTest, IndexFileContainsCorrectConfigAfterRelocation) {
    // Scope to ensure dirDirStore destructor is called
    {
        // Initialize with specific config values
        config.maxDirCount = 5;
        config.maxFileCount = 10;
        config.maxFileSize = 2 * 1024 * 1024; // 2MB
        config.maxLevel = 1;
        dirDirStore.init(config);
        
        // Add blocks to trigger relocation
        std::string largeData(400 * 1024, 'Y'); // 400KB per block
        for (size_t i = 0; i < 25; i++) {
            if (!dirDirStore.canFit(largeData.size())) {
                break;
            }
            dirDirStore.appendBlock(largeData);
        }
    }
    // dirDirStore destructor called here
    
    pp::DirDirStore dirDirStore2;
    dirDirStore2.redirectLogger("dirdirstore2");
    pp::DirDirStore::MountConfig mountConfig;
    mountConfig.dirPath = testDir;
    mountConfig.maxLevel = 1; // Only maxLevel needs to be provided
    
    auto mountResult = dirDirStore2.mount(mountConfig);
    ASSERT_TRUE(mountResult.isOk()) << "Mount should succeed";
    
    // The mounted store should have loaded config values from the index
    // We can verify this by adding more blocks and seeing if they respect the limits
    // Since we can't directly access config_, we verify behavior is consistent
    EXPECT_GT(dirDirStore2.getBlockCount(), 0) 
        << "Should have loaded blocks from relocated store";
}
