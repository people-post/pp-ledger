#include "Ledger.h"
#include <gtest/gtest.h>

class LedgerTest : public ::testing::Test {
protected:
    pp::Ledger* ledger;
    
    void SetUp() override {
        ledger = new pp::Ledger();
    }
    
    void TearDown() override {
        delete ledger;
    }

    // Helper function to create a simple validator that always returns true
    static pp::Ledger::Roe<bool> simpleValidator(const pp::iii::Block&, const pp::iii::BlockChain&) {
        return true;
    }
};

// Test blockchain operations (public methods)
TEST_F(LedgerTest, ProducesBlockWithEmptyTransactionsFails) {
    // Try to produce a block with no pending transactions
    auto result = ledger->produceBlock(0, "leader", simpleValidator);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 1); // Error code for no pending transactions
}

TEST_F(LedgerTest, ValidatesEmptyBlockchain) {
    // Empty blockchain is invalid (BlockChain::isValid() returns false for empty chain)
    EXPECT_FALSE(ledger->isValid());
}

TEST_F(LedgerTest, GetBlockCountInitiallyZero) {
    // Ledger starts with no blocks (no auto-genesis block)
    EXPECT_EQ(ledger->getBlockCount(), 0);
}

TEST_F(LedgerTest, GetLatestBlockInitiallyNull) {
    // Ledger starts with no blocks
    auto block = ledger->getLatestBlock();
    EXPECT_EQ(block, nullptr);
}

TEST_F(LedgerTest, GetSizeInitiallyZero) {
    // Ledger starts with no blocks
    EXPECT_EQ(ledger->getSize(), 0);
}

// Test wallet query operations (public methods)
TEST_F(LedgerTest, GetBalanceOfNonExistentWallet) {
    auto balance = ledger->getBalance("NonExistent");
    EXPECT_TRUE(balance.isError());
}

TEST_F(LedgerTest, HasWalletForNonExistentWallet) {
    EXPECT_FALSE(ledger->hasWallet("NonExistent"));
}

// Test transfer operation (public method) - will fail if wallets don't exist
TEST_F(LedgerTest, TransferRequiresWallets) {
    // Transfer will fail because wallets don't exist (can't create them publicly)
    pp::Ledger::Transaction tx("Alice", "Bob", 100);
    auto result = ledger->addTransaction(tx);
    EXPECT_TRUE(result.isError());
}
