#include "BlockFile.h"
#include "Logger.h"

#include <iostream>
#include <cstring>
#include <filesystem>

int main() {
    auto& logger = pp::logging::getLogger("blockfile_test");
    
    std::cout << "=== Testing BlockFile ===\n\n";
    
    // Setup test directory
    std::string testDir = "/tmp/pp-ledger-test";
    std::string testFile = testDir + "/test_block.dat";
    
    // Clean up from previous tests
    if (std::filesystem::exists(testFile)) {
        std::filesystem::remove(testFile);
    }
    if (!std::filesystem::exists(testDir)) {
        std::filesystem::create_directories(testDir);
    }
    
    // Test 1: Initialize BlockFile
    std::cout << "1. Testing BlockFile initialization:\n";
    pp::BlockFile blockFile;
    pp::BlockFile::Config config(testFile, 1024 * 1024); // 1MB max size
    
    auto initResult = blockFile.init(config);
    if (initResult.isOk()) {
        logger.info << "BlockFile initialized successfully";
        std::cout << "  ✓ BlockFile initialized successfully\n";
    } else {
        logger.error << "Failed to initialize BlockFile: " << initResult.error().message;
        std::cout << "  ✗ Failed to initialize: " << initResult.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    // Test 2: Write data
    std::cout << "2. Testing write operation:\n";
    const char* testData1 = "Hello, BlockFile!";
    size_t dataSize1 = strlen(testData1) + 1;
    
    auto writeResult1 = blockFile.write(testData1, dataSize1);
    if (writeResult1.isOk()) {
        logger.info << "Wrote " << dataSize1 << " bytes at offset " << writeResult1.value();
        std::cout << "  ✓ Wrote " << dataSize1 << " bytes at offset " << writeResult1.value() << "\n";
    } else {
        logger.error << "Write failed: " << writeResult1.error().message;
        std::cout << "  ✗ Write failed: " << writeResult1.error().message << "\n";
        return 1;
    }
    
    int64_t offset1 = writeResult1.value();
    std::cout << "\n";
    
    // Test 3: Write more data
    std::cout << "3. Testing second write operation:\n";
    const char* testData2 = "This is another block of data.";
    size_t dataSize2 = strlen(testData2) + 1;
    
    auto writeResult2 = blockFile.write(testData2, dataSize2);
    if (writeResult2.isOk()) {
        logger.info << "Wrote " << dataSize2 << " bytes at offset " << writeResult2.value();
        std::cout << "  ✓ Wrote " << dataSize2 << " bytes at offset " << writeResult2.value() << "\n";
    } else {
        logger.error << "Write failed: " << writeResult2.error().message;
        std::cout << "  ✗ Write failed: " << writeResult2.error().message << "\n";
        return 1;
    }
    
    int64_t offset2 = writeResult2.value();
    std::cout << "\n";
    
    // Test 4: Read first data back
    std::cout << "4. Testing read operation (first data):\n";
    char readBuffer1[256] = {0};
    
    auto readResult1 = blockFile.read(offset1, readBuffer1, dataSize1);
    if (readResult1.isOk()) {
        logger.info << "Read " << readResult1.value() << " bytes from offset " << offset1;
        std::cout << "  ✓ Read " << readResult1.value() << " bytes\n";
        std::cout << "  Data: \"" << readBuffer1 << "\"\n";
        
        if (strcmp(readBuffer1, testData1) == 0) {
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
    
    // Test 5: Read second data back
    std::cout << "5. Testing read operation (second data):\n";
    char readBuffer2[256] = {0};
    
    auto readResult2 = blockFile.read(offset2, readBuffer2, dataSize2);
    if (readResult2.isOk()) {
        logger.info << "Read " << readResult2.value() << " bytes from offset " << offset2;
        std::cout << "  ✓ Read " << readResult2.value() << " bytes\n";
        std::cout << "  Data: \"" << readBuffer2 << "\"\n";
        
        if (strcmp(readBuffer2, testData2) == 0) {
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
    
    // Test 6: Check canFit
    std::cout << "6. Testing canFit:\n";
    size_t hugeSizeToTest = 2 * 1024 * 1024; // 2MB (larger than max)
    if (blockFile.canFit(hugeSizeToTest)) {
        std::cout << "  ✗ canFit incorrectly returned true for oversized data\n";
    } else {
        std::cout << "  ✓ canFit correctly returned false for oversized data\n";
    }
    std::cout << "\n";
    
    // Test 7: Flush
    std::cout << "7. Testing flush:\n";
    blockFile.flush();
    logger.info << "Flushed BlockFile";
    std::cout << "  ✓ Flushed successfully\n";
    std::cout << "\n";
    
    // Test 8: Reopen existing file
    std::cout << "8. Testing reopen of existing file:\n";
    pp::BlockFile blockFile2;
    auto initResult2 = blockFile2.init(config);
    if (initResult2.isOk()) {
        logger.info << "Reopened existing BlockFile";
        std::cout << "  ✓ Reopened existing file successfully\n";
        
        // Verify we can still read the data
        char readBuffer3[256] = {0};
        auto readResult3 = blockFile2.read(offset1, readBuffer3, dataSize1);
        if (readResult3.isOk() && strcmp(readBuffer3, testData1) == 0) {
            std::cout << "  ✓ Data persisted correctly\n";
        } else {
            std::cout << "  ✗ Data not persisted correctly\n";
            return 1;
        }
    } else {
        logger.error << "Failed to reopen: " << initResult2.error().message;
        std::cout << "  ✗ Failed to reopen: " << initResult2.error().message << "\n";
        return 1;
    }
    std::cout << "\n";
    
    std::cout << "=== All BlockFile tests passed! ===\n";
    
    // Cleanup
    std::filesystem::remove(testFile);
    
    return 0;
}
