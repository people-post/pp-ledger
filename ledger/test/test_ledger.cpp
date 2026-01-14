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
};

// Test blockchain operations (public methods)
TEST_F(LedgerTest, CommitsEmptyTransactionsFails) {
    auto result = ledger->commitTransactions();
    EXPECT_TRUE(result.isError());
}

TEST_F(LedgerTest, ValidatesBlockchain) {
    EXPECT_TRUE(ledger->isValid());
}

TEST_F(LedgerTest, GetBlockCount) {
    EXPECT_EQ(ledger->getBlockCount(), 1); // Genesis block
}

TEST_F(LedgerTest, GetLatestBlock) {
    auto block = ledger->getLatestBlock();
    EXPECT_NE(block, nullptr);
    EXPECT_EQ(block->getIndex(), 0); // Genesis block
}

TEST_F(LedgerTest, GetSize) {
    EXPECT_EQ(ledger->getSize(), 1); // Genesis block
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
