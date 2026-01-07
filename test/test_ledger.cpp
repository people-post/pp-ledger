#include "Ledger.h"
#include "Logger.h"

#include <iostream>

int main() {
    auto& logger = pp::logging::getLogger("ledger_test");
    
    std::cout << "=== Testing Ledger (Wallet & Transactions) ===\n\n";
    
    // Test 1: Create bank and wallets
    std::cout << "1. Creating ledger with difficulty 2...\n";
    pp::Ledger ledger(2);
    
    logger.info << "Creating wallets...";
    auto result1 = ledger.createWallet("Alice");
    auto result2 = ledger.createWallet("Bob");
    auto result3 = ledger.createWallet("Charlie");
    
    if (result1.isOk() && result2.isOk() && result3.isOk()) {
        std::cout << "✓ Created wallets: Alice, Bob, Charlie\n\n";
    } else {
        std::cout << "✗ Failed to create wallets\n";
        return 1;
    }
    
    // Test 2: Deposit to wallets
    std::cout << "2. Depositing funds...\n";
    ledger.deposit("Alice", 1000);
    ledger.deposit("Bob", 500);
    ledger.deposit("Charlie", 250);
    
    std::cout << "  Pending transactions: " << ledger.getPendingTransactionCount() << "\n";
    std::cout << "  Alice: " << ledger.getBalance("Alice").value() << "\n";
    std::cout << "  Bob: " << ledger.getBalance("Bob").value() << "\n";
    std::cout << "  Charlie: " << ledger.getBalance("Charlie").value() << "\n\n";
    
    // Test 3: Commit first block
    std::cout << "3. Committing deposit transactions to blockchain...\n";
    logger.info << "Mining block...";
    auto commitResult = ledger.commitTransactions();
    if (commitResult.isOk()) {
        std::cout << "✓ Block mined and added to chain\n";
        std::cout << "  Blocks in chain: " << ledger.getBlockCount() << "\n";
        std::cout << "  Pending transactions: " << ledger.getPendingTransactionCount() << "\n\n";
    } else {
        std::cout << "✗ Failed to commit: " << commitResult.error() << "\n\n";
    }
    
    // Test 4: Transfer between wallets
    std::cout << "4. Making transfers...\n";
    ledger.transfer("Alice", "Bob", 200);
    ledger.transfer("Bob", "Charlie", 150);
    ledger.transfer("Alice", "Charlie", 100);
    
    std::cout << "  Pending transactions: " << ledger.getPendingTransactionCount() << "\n";
    std::cout << "  Alice: " << ledger.getBalance("Alice").value() << "\n";
    std::cout << "  Bob: " << ledger.getBalance("Bob").value() << "\n";
    std::cout << "  Charlie: " << ledger.getBalance("Charlie").value() << "\n\n";
    
    // Test 5: Commit transfer block
    std::cout << "5. Committing transfer transactions to blockchain...\n";
    logger.info << "Mining block...";
    auto commitResult2 = ledger.commitTransactions();
    if (commitResult2.isOk()) {
        std::cout << "✓ Block mined and added to chain\n";
        std::cout << "  Blocks in chain: " << ledger.getBlockCount() << "\n\n";
    }
    
    // Test 6: Withdrawal
    std::cout << "6. Making withdrawals...\n";
    auto withdrawResult = ledger.withdraw("Bob", 300);
    if (withdrawResult.isOk()) {
        std::cout << "✓ Bob withdrew 300\n";
        std::cout << "  Bob's balance: " << ledger.getBalance("Bob").value() << "\n";
    } else {
        std::cout << "✗ Withdrawal failed: " << withdrawResult.error() << "\n";
    }
    
    auto withdrawResult2 = ledger.withdraw("Charlie", 1000);
    if (withdrawResult2.isError()) {
        std::cout << "✓ Charlie couldn't withdraw 1000 (insufficient funds)\n";
        std::cout << "  Error: " << withdrawResult2.error() << "\n";
    }
    std::cout << "\n";
    
    // Test 7: Display blockchain
    std::cout << "7. Displaying blockchain:\n";
    const auto& blockchain = ledger.getBlockChain();
    const auto& chain = blockchain.getChain();
    
    for (const auto& block : chain) {
        std::cout << "Block #" << block.index << ":\n";
        std::cout << "  Data:\n" << block.data;
        std::cout << "  Hash: " << block.hash.substr(0, 16) << "...\n";
        std::cout << "  Nonce: " << block.nonce << "\n\n";
    }
    
    // Test 8: Validate blockchain
    std::cout << "8. Validating blockchain...\n";
    if (ledger.isValid()) {
        logger.info << "Blockchain is valid";
        std::cout << "✓ Blockchain is valid!\n\n";
    } else {
        logger.error << "Blockchain is invalid";
        std::cout << "✗ Blockchain is invalid!\n\n";
    }
    
    // Test 9: Error handling
    std::cout << "9. Testing error handling...\n";
    auto result = ledger.createWallet("Alice");
    if (result.isError()) {
        std::cout << "✓ Cannot create duplicate wallet\n";
        std::cout << "  Error: " << result.error() << "\n";
    }
    
    auto balanceResult = ledger.getBalance("NonExistent");
    if (balanceResult.isError()) {
        std::cout << "✓ Cannot get balance of non-existent wallet\n";
        std::cout << "  Error: " << balanceResult.error() << "\n";
    }
    std::cout << "\n";
    
    logger.info << "Ledger test complete";
    std::cout << "=== Test Complete ===\n";
    
    return 0;
}
