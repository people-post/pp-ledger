#include "Ledger.h"
#include <gtest/gtest.h>

class LedgerTest : public ::testing::Test {
protected:
    pp::Ledger* ledger;
    
    void SetUp() override {
        ledger = new pp::Ledger(2);
    }
    
    void TearDown() override {
        delete ledger;
    }
};

TEST_F(LedgerTest, CreatesWallets) {
    auto result1 = ledger->createWallet("Alice");
    auto result2 = ledger->createWallet("Bob");
    auto result3 = ledger->createWallet("Charlie");
    
    EXPECT_TRUE(result1.isOk());
    EXPECT_TRUE(result2.isOk());
    EXPECT_TRUE(result3.isOk());
}

TEST_F(LedgerTest, RejectsDuplicateWallet) {
    ledger->createWallet("Alice");
    auto result = ledger->createWallet("Alice");
    EXPECT_TRUE(result.isError());
}

TEST_F(LedgerTest, DepositIncreasesBalance) {
    ledger->createWallet("Alice");
    ledger->deposit("Alice", 1000);
    
    auto balance = ledger->getBalance("Alice");
    EXPECT_TRUE(balance.isOk());
    EXPECT_EQ(balance.value(), 1000);
}

TEST_F(LedgerTest, CommitsTransactionsToBlockchain) {
    ledger->createWallet("Alice");
    ledger->deposit("Alice", 1000);
    
    EXPECT_GT(ledger->getPendingTransactionCount(), 0);
    
    auto result = ledger->commitTransactions();
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(ledger->getBlockCount(), 2); // Genesis + 1 block
    EXPECT_EQ(ledger->getPendingTransactionCount(), 0);
}

TEST_F(LedgerTest, TransferBetweenWallets) {
    ledger->createWallet("Alice");
    ledger->createWallet("Bob");
    
    ledger->deposit("Alice", 1000);
    ledger->commitTransactions();
    
    ledger->transfer("Alice", "Bob", 200);
    ledger->commitTransactions();
    
    EXPECT_EQ(ledger->getBalance("Alice").value(), 800);
    EXPECT_EQ(ledger->getBalance("Bob").value(), 200);
}

TEST_F(LedgerTest, WithdrawDecreasesBalance) {
    ledger->createWallet("Bob");
    ledger->deposit("Bob", 500);
    ledger->commitTransactions();
    
    auto result = ledger->withdraw("Bob", 300);
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(ledger->getBalance("Bob").value(), 200);
}

TEST_F(LedgerTest, RejectsOverdraft) {
    ledger->createWallet("Charlie");
    ledger->deposit("Charlie", 250);
    ledger->commitTransactions();
    
    auto result = ledger->withdraw("Charlie", 1000);
    EXPECT_TRUE(result.isError());
}

TEST_F(LedgerTest, ValidatesBlockchain) {
    ledger->createWallet("Alice");
    ledger->deposit("Alice", 1000);
    ledger->commitTransactions();
    
    EXPECT_TRUE(ledger->isValid());
}

TEST_F(LedgerTest, GetBalanceOfNonExistentWallet) {
    auto balance = ledger->getBalance("NonExistent");
    EXPECT_TRUE(balance.isError());
}
