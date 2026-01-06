#include "Ledger.h"
#include "Logger.h"

#include <iostream>

int main() {
    auto& logger = pp::logging::getLogger("ledger_test");
    
    std::cout << "=== Testing Blockchain Ledger ===\n\n";
    
    // Test 1: Create ledger and check genesis block
    std::cout << "1. Creating ledger with difficulty 2...\n";
    pp::Ledger ledger(2);
    logger.info << "Ledger created with " << ledger.getSize() << " block(s)";
    
    const auto& genesis = ledger.getBlock(0);
    std::cout << "Genesis Block:\n";
    std::cout << "  Index: " << genesis.index << "\n";
    std::cout << "  Hash: " << genesis.hash << "\n";
    std::cout << "  Previous Hash: " << genesis.previousHash << "\n";
    std::cout << "  Nonce: " << genesis.nonce << "\n\n";
    
    // Test 2: Add blocks
    std::cout << "2. Adding blocks to the chain...\n";
    logger.info << "Adding block 1...";
    ledger.addBlock("Transaction 1: Alice -> Bob: 10 coins");
    
    logger.info << "Adding block 2...";
    ledger.addBlock("Transaction 2: Bob -> Charlie: 5 coins");
    
    logger.info << "Adding block 3...";
    ledger.addBlock("Transaction 3: Charlie -> Alice: 3 coins");
    
    std::cout << "Chain now has " << ledger.getSize() << " blocks\n\n";
    
    // Test 3: Display the chain
    std::cout << "3. Displaying blockchain:\n";
    const auto& chain = ledger.getChain();
    for (const auto& block : chain) {
        std::cout << "Block #" << block.index << ":\n";
        std::cout << "  Data: " << block.data << "\n";
        std::cout << "  Hash: " << block.hash << "\n";
        std::cout << "  Previous Hash: " << block.previousHash << "\n";
        std::cout << "  Nonce: " << block.nonce << "\n";
        std::cout << "  Timestamp: " << block.timestamp << "\n\n";
    }
    
    // Test 4: Validate the chain
    std::cout << "4. Validating blockchain...\n";
    bool isValid = ledger.isValid();
    if (isValid) {
        logger.info << "Blockchain is VALID";
        std::cout << "✓ Blockchain is valid!\n\n";
    } else {
        logger.error << "Blockchain is INVALID";
        std::cout << "✗ Blockchain is invalid!\n\n";
    }
    
    // Test 5: Try tampering with a block
    std::cout << "5. Testing tampering detection...\n";
    std::cout << "Attempting to tamper with block 1...\n";
    pp::Ledger tamperedLedger(2);
    tamperedLedger.addBlock("Original Transaction");
    tamperedLedger.addBlock("Another Transaction");
    
    // Manually tamper with the chain (this is for testing only)
    auto& tamperedChain = const_cast<std::vector<pp::Block>&>(tamperedLedger.getChain());
    tamperedChain[1].data = "Tampered Transaction!!!";
    
    std::cout << "Validating tampered blockchain...\n";
    bool isTamperedValid = tamperedLedger.isValid();
    if (isTamperedValid) {
        logger.warning << "Tampered blockchain appears VALID (unexpected!)";
        std::cout << "✗ Failed to detect tampering!\n\n";
    } else {
        logger.info << "Tampered blockchain detected as INVALID";
        std::cout << "✓ Tampering detected successfully!\n\n";
    }
    
    // Test 6: Different difficulty levels
    std::cout << "6. Testing different difficulty levels...\n";
    for (uint32_t diff = 1; diff <= 4; diff++) {
        pp::Ledger testLedger(diff);
        auto start = std::chrono::high_resolution_clock::now();
        testLedger.addBlock("Test block");
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        const auto& block = testLedger.getLatestBlock();
        logger.info << "Difficulty " << diff << ": mined in " << duration.count() << "ms, nonce=" << block.nonce;
        std::cout << "Difficulty " << diff << ": " << duration.count() << "ms (nonce: " << block.nonce << ")\n";
    }
    
    std::cout << "\n=== Test Complete ===\n";
    
    return 0;
}
