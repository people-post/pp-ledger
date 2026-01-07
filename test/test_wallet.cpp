#include "Wallet.h"
#include <gtest/gtest.h>

TEST(WalletTest, DefaultConstructorCreatesZeroBalance) {
    pp::Wallet wallet;
    EXPECT_EQ(wallet.getBalance(), 0);
}

TEST(WalletTest, ConstructorWithInitialBalance) {
    pp::Wallet wallet(1000);
    EXPECT_EQ(wallet.getBalance(), 1000);
}

TEST(WalletTest, DepositIncreasesBalance) {
    pp::Wallet wallet;
    auto result = wallet.deposit(500);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(wallet.getBalance(), 500);
}

TEST(WalletTest, NegativeDepositRejected) {
    pp::Wallet wallet;
    auto result = wallet.deposit(-100);
    EXPECT_TRUE(result.isError());
}

TEST(WalletTest, WithdrawDecreasesBalance) {
    pp::Wallet wallet(1000);
    auto result = wallet.withdraw(300);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(wallet.getBalance(), 700);
}

TEST(WalletTest, OverdraftRejected) {
    pp::Wallet wallet(500);
    auto result = wallet.withdraw(1000);
    EXPECT_TRUE(result.isError());
}

TEST(WalletTest, TransferSucceeds) {
    pp::Wallet wallet1(500);
    pp::Wallet wallet2(700);
    
    auto result = wallet1.transfer(wallet2, 200);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(wallet1.getBalance(), 300);
    EXPECT_EQ(wallet2.getBalance(), 900);
}

TEST(WalletTest, TransferWithInsufficientBalance) {
    pp::Wallet wallet1(300);
    pp::Wallet wallet2;
    
    auto result = wallet1.transfer(wallet2, 1000);
    EXPECT_TRUE(result.isError());
}

TEST(WalletTest, HasBalancePositive) {
    pp::Wallet wallet(500);
    EXPECT_TRUE(wallet.hasBalance(100));
    EXPECT_TRUE(wallet.hasBalance(500));
    EXPECT_FALSE(wallet.hasBalance(600));
}

TEST(WalletTest, IsEmptyReturnsTrueForZeroBalance) {
    pp::Wallet wallet;
    EXPECT_TRUE(wallet.isEmpty());
}

TEST(WalletTest, IsEmptyReturnsFalseForNonZeroBalance) {
    pp::Wallet wallet(100);
    EXPECT_FALSE(wallet.isEmpty());
}

TEST(WalletTest, ResetSetsBalanceToZero) {
    pp::Wallet wallet(1000);
    wallet.reset();
    EXPECT_EQ(wallet.getBalance(), 0);
}

TEST(WalletTest, SetBalanceChangesBalance) {
    pp::Wallet wallet;
    wallet.setBalance(5000);
    EXPECT_EQ(wallet.getBalance(), 5000);
}

TEST(WalletTest, OverflowProtection) {
    pp::Wallet wallet(INT64_MAX);
    auto result = wallet.deposit(1);
    EXPECT_TRUE(result.isError());
}

TEST(WalletTest, MultipleOperations) {
    pp::Wallet account(1000);
    
    account.deposit(500);
    EXPECT_EQ(account.getBalance(), 1500);
    
    account.withdraw(200);
    EXPECT_EQ(account.getBalance(), 1300);
    
    account.deposit(300);
    EXPECT_EQ(account.getBalance(), 1600);
}
