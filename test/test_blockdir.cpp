#include "BlockDir.h"
#include "Logger.h"

#include <iostream>
#include <cstring>
#include <filesystem>

int main() {
    auto& logger = pp::logging::getLogger("blockdir_test");
    
    std::cout << "=== Testing BlockDir ===\n\n";
    
    // Setup test directory
    std::string testDir = "/tmp/pp-ledger-blockdir-test";
    
    // Clean up from previous tests
    if (std::filesystem::exists(testDir)) {
        std::filesystem::remove_all(testDir);
    }
    
    // Test 1: Initialize BlockDir
    std::cout << "1. Testing BlockDir initialization:\n";
    pp::BlockDir blockDir;
    pp::BlockDir::Config config(testDir, 100); // Very small max file size to test multiple files
    
    auto initResult = blockDir.init(config);
    if (initResult.isOk()) {
        logger.info << "BlockDir initialized successfully";
        std::cout << "  ✓ BlockDir initialized successfully\n";
    } else {
        logger.error << "Failed to initialize BlockDir: " << initResult.error().message;
        std::cout << "  ✗ Failed to initialize: " << initResult.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 2: Write first block
    std::cout << "2. Testing write first block:\n";
    const char* blockData1 = "Block #1: First block of data";
    size_t blockSize1 = strlen(blockData1) + 1;
    uint64_t blockId1 = 1001;
    
    auto writeResult1 = blockDir.writeBlock(blockId1, blockData1, blockSize1);
    if (writeResult1.isOk()) {
        logger.info << "Wrote block " << blockId1 << " (" << blockSize1 << " bytes)";
        std::cout << "  ✓ Wrote block " << blockId1 << " (" << blockSize1 << " bytes)\n";
    } else {
        logger.error << "Write failed: " << writeResult1.error().message;
        std::cout << "  ✗ Write failed: " << writeResult1.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 3: Write second block
    std::cout << "3. Testing write second block:\n";
    const char* blockData2 = "Block #2: Another block";
    size_t blockSize2 = strlen(blockData2) + 1;
    uint64_t blockId2 = 1002;
    
    auto writeResult2 = blockDir.writeBlock(blockId2, blockData2, blockSize2);
    if (writeResult2.isOk()) {
        logger.info << "Wrote block " << blockId2 << " (" << blockSize2 << " bytes)";
        std::cout << "  ✓ Wrote block " << blockId2 << " (" << blockSize2 << " bytes)\n";
    } else {
        logger.error << "Write failed: " << writeResult2.error().message;
        std::cout << "  ✗ Write failed: " << writeResult2.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 4: Write third block (should trigger new file due to small max size)
    std::cout << "4. Testing write third block (new file):\n";
    const char* blockData3 = "Block #3: This should go in a new file";
    size_t blockSize3 = strlen(blockData3) + 1;
    uint64_t blockId3 = 1003;
    
    auto writeResult3 = blockDir.writeBlock(blockId3, blockData3, blockSize3);
    if (writeResult3.isOk()) {
        logger.info << "Wrote block " << blockId3 << " (" << blockSize3 << " bytes)";
        std::cout << "  ✓ Wrote block " << blockId3 << " (" << blockSize3 << " bytes)\n";
    } else {
        logger.error << "Write failed: " << writeResult3.error().message;
        std::cout << "  ✗ Write failed: " << writeResult3.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 5: Check hasBlock
    std::cout << "5. Testing hasBlock:\n";
    if (blockDir.hasBlock(blockId1)) {
        std::cout << "  ✓ Block " << blockId1 << " exists\n";
    } else {
        std::cout << "  ✗ Block " << blockId1 << " should exist\n";
        return 1;
    }
    
    if (!blockDir.hasBlock(9999)) {
        std::cout << "  ✓ Block 9999 doesn't exist (as expected)\n";
    } else {
        std::cout << "  ✗ Block 9999 shouldn't exist\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 6: Read first block
    std::cout << "6. Testing read first block:\n";
    char readBuffer1[256] = {0};
    
    auto readResult1 = blockDir.readBlock(blockId1, readBuffer1, sizeof(readBuffer1));
    if (readResult1.isOk()) {
        logger.info << "Read block " << blockId1 << " (" << readResult1.value() << " bytes)";
        std::cout << "  ✓ Read block " << blockId1 << " (" << readResult1.value() << " bytes)\n";
        std::cout << "  Data: \"" << readBuffer1 << "\"\n";
        
        if (strcmp(readBuffer1, blockData1) == 0) {
            std::cout << "  ✓ Data matches original\n";
        } else {
            std::cout << "  ✗ Data mismatch!\n";
            return 1;
        }
    } else {
        logger.error << "Read failed: " << readResult1.error().message;
        std::cout << "  ✗ Read failed: " << readResult1.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 7: Read second block
    std::cout << "7. Testing read second block:\n";
    char readBuffer2[256] = {0};
    
    auto readResult2 = blockDir.readBlock(blockId2, readBuffer2, sizeof(readBuffer2));
    if (readResult2.isOk()) {
        logger.info << "Read block " << blockId2 << " (" << readResult2.value() << " bytes)";
        std::cout << "  ✓ Read block " << blockId2 << " (" << readResult2.value() << " bytes)\n";
        std::cout << "  Data: \"" << readBuffer2 << "\"\n";
        
        if (strcmp(readBuffer2, blockData2) == 0) {
            std::cout << "  ✓ Data matches original\n";
        } else {
            std::cout << "  ✗ Data mismatch!\n";
            return 1;
        }
    } else {
        logger.error << "Read failed: " << readResult2.error().message;
        std::cout << "  ✗ Read failed: " << readResult2.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 8: Read third block
    std::cout << "8. Testing read third block:\n";
    char readBuffer3[256] = {0};
    
    auto readResult3 = blockDir.readBlock(blockId3, readBuffer3, sizeof(readBuffer3));
    if (readResult3.isOk()) {
        logger.info << "Read block " << blockId3 << " (" << readResult3.value() << " bytes)";
        std::cout << "  ✓ Read block " << blockId3 << " (" << readResult3.value() << " bytes)\n";
        std::cout << "  Data: \"" << readBuffer3 << "\"\n";
        
        if (strcmp(readBuffer3, blockData3) == 0) {
            std::cout << "  ✓ Data matches original\n";
        } else {
            std::cout << "  ✗ Data mismatch!\n";
            return 1;
        }
    } else {
        logger.error << "Read failed: " << readResult3.error().message;
        std::cout << "  ✗ Read failed: " << readResult3.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 9: Attempt to write duplicate block
    std::cout << "9. Testing duplicate block write (should fail):\n";
    auto writeResult4 = blockDir.writeBlock(blockId1, blockData1, blockSize1);
    if (writeResult4.isOk()) {
        std::cout << "  ✗ Duplicate write should have failed\n";
        return 1;
    } else {
        logger.info << "Duplicate write correctly rejected: " << writeResult4.error().message;
        std::cout << "  ✓ Duplicate write rejected: " << writeResult4.error().message << "\n";
    }
    std::cout << "\n";
    
    // Test 10: Flush
    std::cout << "10. Testing flush:\n";
    blockDir.flush();
    logger.info << "Flushed BlockDir";
    std::cout << "  ✓ Flushed successfully\n";
    std::cout << "\n";
    
    // Test 11: Reopen and verify persistence
    std::cout << "11. Testing reopen and persistence:\n";
    pp::BlockDir blockDir2;
    auto initResult2 = blockDir2.init(config);
    if (initResult2.isOk()) {
        logger.info << "Reopened BlockDir";
        std::cout << "  ✓ Reopened BlockDir successfully\n";
        
        // Verify blocks still exist
        if (blockDir2.hasBlock(blockId1) && blockDir2.hasBlock(blockId2) && blockDir2.hasBlock(blockId3)) {
            std::cout << "  ✓ All blocks still exist\n";
            
            // Verify we can read the data
            char verifyBuffer[256] = {0};
            auto verifyResult = blockDir2.readBlock(blockId2, verifyBuffer, sizeof(verifyBuffer));
            if (verifyResult.isOk() && strcmp(verifyBuffer, blockData2) == 0) {
                std::cout << "  ✓ Data persisted correctly\n";
            } else {
                std::cout << "  ✗ Data not persisted correctly\n";
                return 1;
            }
        } else {
            std::cout << "  ✗ Not all blocks persisted\n";
            return 1;
        }
    } else {
        logger.error << "Failed to reopen: " << initResult2.error().message;
        std::cout << "  ✗ Failed to reopen: " << initResult2.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 12: Read non-existent block
    std::cout << "12. Testing read non-existent block:\n";
    char dummyBuffer[256];
    auto readResult4 = blockDir2.readBlock(99999, dummyBuffer, sizeof(dummyBuffer));
    if (readResult4.isOk()) {
        std::cout << "  ✗ Read of non-existent block should have failed\n";
        return 1;
    } else {
        logger.info << "Read of non-existent block correctly failed: " << readResult4.error().message;
        std::cout << "  ✓ Read failed as expected: " << readResult4.error().message << "\n";
    }
    std::cout << "\n";
    
    std::cout << "=== All BlockDir tests passed! ===\n";
    
    // Cleanup
    std::filesystem::remove_all(testDir);
    
    return 0;
}
