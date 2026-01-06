#include "Wallet.h"
#include "Logger.h"

#include <iostream>

int main() {
    auto& logger = pp::logging::getLogger("wallet_test");
    
    std::cout << "=== Testing Wallet ===\n\n";
    
    // Test 1: Create wallet and check initial balance
    std::cout << "1. Creating wallets:\n";
    pp::Wallet wallet1;
    pp::Wallet wallet2(1000);
    
    logger.info << "Wallet1 balance: " << wallet1.getBalance();
    logger.info << "Wallet2 balance: " << wallet2.getBalance();
    std::cout << "  Wallet1 (default): " << wallet1.getBalance() << "\n";
    std::cout << "  Wallet2 (1000): " << wallet2.getBalance() << "\n\n";
    
    // Test 2: Deposit
    std::cout << "2. Testing deposit:\n";
    auto result1 = wallet1.deposit(500);
    if (result1.isOk()) {
        logger.info << "Deposited 500 to wallet1, new balance: " << wallet1.getBalance();
        std::cout << "  ✓ Deposited 500, new balance: " << wallet1.getBalance() << "\n";
    } else {
        logger.error << "Deposit failed: " << result1.error();
        std::cout << "  ✗ Deposit failed: " << result1.error() << "\n";
    }
    
    auto result2 = wallet1.deposit(-100);
    if (result2.isOk()) {
        std::cout << "  ✗ Negative deposit succeeded (should have failed)\n";
    } else {
        logger.info << "Negative deposit correctly rejected: " << result2.error();
        std::cout << "  ✓ Negative deposit rejected: " << result2.error() << "\n";
    }
    std::cout << "\n";
    
    // Test 3: Withdrawal
    std::cout << "3. Testing withdrawal:\n";
    auto result3 = wallet2.withdraw(300);
    if (result3.isOk()) {
        logger.info << "Withdrew 300 from wallet2, new balance: " << wallet2.getBalance();
        std::cout << "  ✓ Withdrew 300, new balance: " << wallet2.getBalance() << "\n";
    } else {
        std::cout << "  ✗ Withdrawal failed: " << result3.error() << "\n";
    }
    
    auto result4 = wallet2.withdraw(1000);
    if (result4.isOk()) {
        std::cout << "  ✗ Overdraft succeeded (should have failed)\n";
    } else {
        logger.info << "Overdraft correctly rejected: " << result4.error();
        std::cout << "  ✓ Overdraft rejected: " << result4.error() << "\n";
    }
    std::cout << "\n";
    
    // Test 4: Transfer
    std::cout << "4. Testing transfer:\n";
    std::cout << "  Before transfer: wallet1=" << wallet1.getBalance() 
              << ", wallet2=" << wallet2.getBalance() << "\n";
    
    auto result5 = wallet1.transfer(wallet2, 200);
    if (result5.isOk()) {
        logger.info << "Transferred 200 from wallet1 to wallet2";
        std::cout << "  ✓ Transferred 200\n";
        std::cout << "  After transfer: wallet1=" << wallet1.getBalance() 
                  << ", wallet2=" << wallet2.getBalance() << "\n";
    } else {
        logger.error << "Transfer failed: " << result5.error();
        std::cout << "  ✗ Transfer failed: " << result5.error() << "\n";
    }
    std::cout << "\n";
    
    // Test 5: Insufficient balance transfer
    std::cout << "5. Testing insufficient balance transfer:\n";
    auto result6 = wallet1.transfer(wallet2, 1000);
    if (result6.isOk()) {
        std::cout << "  ✗ Insufficient balance transfer succeeded (should have failed)\n";
    } else {
        logger.info << "Insufficient balance transfer rejected: " << result6.error();
        std::cout << "  ✓ Transfer rejected: " << result6.error() << "\n";
    }
    std::cout << "\n";
    
    // Test 6: Query operations
    std::cout << "6. Testing query operations:\n";
    std::cout << "  wallet1.hasBalance(100): " << (wallet1.hasBalance(100) ? "true" : "false") << "\n";
    std::cout << "  wallet1.hasBalance(500): " << (wallet1.hasBalance(500) ? "true" : "false") << "\n";
    std::cout << "  wallet1.isEmpty(): " << (wallet1.isEmpty() ? "true" : "false") << "\n";
    
    pp::Wallet emptyWallet;
    std::cout << "  emptyWallet.isEmpty(): " << (emptyWallet.isEmpty() ? "true" : "false") << "\n";
    std::cout << "\n";
    
    // Test 7: Reset and setBalance
    std::cout << "7. Testing reset and setBalance:\n";
    std::cout << "  Before reset: wallet2=" << wallet2.getBalance() << "\n";
    wallet2.reset();
    logger.info << "Wallet2 reset, balance: " << wallet2.getBalance();
    std::cout << "  After reset: wallet2=" << wallet2.getBalance() << "\n";
    
    wallet2.setBalance(5000);
    logger.info << "Wallet2 balance set to: " << wallet2.getBalance();
    std::cout << "  After setBalance(5000): wallet2=" << wallet2.getBalance() << "\n";
    std::cout << "\n";
    
    // Test 8: Overflow protection
    std::cout << "8. Testing overflow protection:\n";
    pp::Wallet maxWallet(INT64_MAX);
    auto result7 = maxWallet.deposit(1);
    if (result7.isOk()) {
        std::cout << "  ✗ Overflow deposit succeeded (should have failed)\n";
    } else {
        logger.info << "Overflow protection working: " << result7.error();
        std::cout << "  ✓ Overflow prevented: " << result7.error() << "\n";
    }
    
    // Test 9: Multiple operations
    std::cout << "\n9. Testing multiple operations:\n";
    pp::Wallet account(1000);
    logger.info << "Starting balance: " << account.getBalance();
    
    account.deposit(500);
    logger.info << "After deposit(500): " << account.getBalance();
    
    account.withdraw(200);
    logger.info << "After withdraw(200): " << account.getBalance();
    
    account.deposit(300);
    logger.info << "After deposit(300): " << account.getBalance();
    
    std::cout << "  Final balance: " << account.getBalance() << "\n";
    std::cout << "  Expected: 1600, Actual: " << account.getBalance();
    std::cout << (account.getBalance() == 1600 ? " ✓" : " ✗") << "\n";
    
    logger.info << "Test complete";
    std::cout << "\n=== Test Complete ===\n";
    
    return 0;
}
