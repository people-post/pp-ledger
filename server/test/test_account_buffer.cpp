#include "../AccountBuffer.h"
#include "../Validator.h"
#include <gtest/gtest.h>
#include <cstdint>

using namespace pp;

class AccountBufferTest : public ::testing::Test {
protected:
    AccountBuffer buf;

    AccountBuffer::Account makeAccount(uint64_t id, int64_t balance) {
        AccountBuffer::Account a;
        a.id = id;
        a.wallet.publicKeys = {"pk-" + std::to_string(id)};
        a.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance; // Use native token (ID ID_GENESIS)
        return a;
    }
};

// --- depositBalance ---

TEST_F(AccountBufferTest, DepositBalance_Success_IncreasesBalance) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.depositBalance(1, AccountBuffer::ID_GENESIS, 50); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.getAccount(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 150);
}

TEST_F(AccountBufferTest, DepositBalance_ZeroAmount_Success) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.depositBalance(1, AccountBuffer::ID_GENESIS, 0); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.getAccount(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, DepositBalance_NegativeAmount_Error) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.depositBalance(1, AccountBuffer::ID_GENESIS, -10); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 10);
    EXPECT_EQ(r.error().message, "Deposit amount must be non-negative");
}

TEST_F(AccountBufferTest, DepositBalance_AccountNotFound_Error) {
    auto r = buf.depositBalance(999, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 9);
    EXPECT_EQ(r.error().message, "Account not found");
}

TEST_F(AccountBufferTest, DepositBalance_Overflow_Error) {
    auto a = makeAccount(1, INT64_MAX);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.depositBalance(1, AccountBuffer::ID_GENESIS, 1); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 11);
    EXPECT_EQ(r.error().message, "Deposit would cause balance overflow");

    auto got = buf.getAccount(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), INT64_MAX);
}

// --- withdrawBalance ---

TEST_F(AccountBufferTest, WithdrawBalance_Success_DecreasesBalance) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 30); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.getAccount(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 70);
}

TEST_F(AccountBufferTest, WithdrawBalance_ZeroAmount_Success) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 0); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.getAccount(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, WithdrawBalance_ExactBalance_Success) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.getAccount(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 0);
}

TEST_F(AccountBufferTest, WithdrawBalance_NegativeAmount_Error) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, -5); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 11);
    EXPECT_EQ(r.error().message, "Withdraw amount must be non-negative");
}

TEST_F(AccountBufferTest, WithdrawBalance_AccountNotFound_Error) {
    auto r = buf.withdrawBalance(999, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 12);
    EXPECT_EQ(r.error().message, "Account not found");
}

TEST_F(AccountBufferTest, WithdrawBalance_InsufficientBalance_Error) {
    auto a = makeAccount(1, 50);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 13);
    EXPECT_EQ(r.error().message, "Insufficient balance");

    auto got = buf.getAccount(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 50);
}

TEST_F(AccountBufferTest, WithdrawBalance_Underflow_Error) {
    auto a = makeAccount(AccountBuffer::ID_GENESIS, INT64_MIN);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(AccountBuffer::ID_GENESIS, AccountBuffer::ID_GENESIS, 1); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 14);
    EXPECT_EQ(r.error().message, "Withdraw would cause balance underflow");

    auto got = buf.getAccount(AccountBuffer::ID_GENESIS);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), INT64_MIN);
}

// --- addTransaction ---

TEST_F(AccountBufferTest, AddTransaction_Success_GenesisToken) {
    // Create source account with balance
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());
    
    // Create destination account
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    // Transfer 100 with 10 fee (both from ID_GENESIS balance)
    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isOk());

    // Check source account: 1000 - 100 - 10 = 890
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 890);

    // Check destination account: 0 + 100 = 100
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, AddTransaction_Success_CustomToken) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    // Set the buffer's tokenId to the custom token
    buf.reset();
    
    // Create source account with both token balances
    AccountBuffer::Account a;
    a.id = 1;
    a.wallet.mBalances[CUSTOM_TOKEN] = 500;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100;
    ASSERT_TRUE(buf.add(a).isOk());
    
    // Create destination account
    AccountBuffer::Account b;
    b.id = 2;
    b.wallet.mBalances[CUSTOM_TOKEN] = 0;
    ASSERT_TRUE(buf.add(b).isOk());

    // Transfer 200 of custom token with 10 fee in ID_GENESIS
    auto r = buf.addTransaction(1, 2, CUSTOM_TOKEN, 200, 10);
    ASSERT_TRUE(r.isOk());

    // Check source account: custom token 500 - 200 = 300, genesis 100 - 10 = 90
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(CUSTOM_TOKEN), 300);
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 90);

    // Check destination account: custom token 0 + 200 = 200
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(CUSTOM_TOKEN), 200);
}

TEST_F(AccountBufferTest, AddTransaction_CreatesDestinationAccount) {
    // Create source account only
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    // Destination account (id=2) doesn't exist yet
    EXPECT_FALSE(buf.hasAccount(2));

    // Transfer should create destination account
    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isOk());

    // Destination account should now exist
    EXPECT_TRUE(buf.hasAccount(2));
    
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().id, 2);
    EXPECT_EQ(to.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, AddTransaction_ZeroAmount_NoOp) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());
    
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    // Zero amount should succeed but do nothing
    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, 0, 10);
    ASSERT_TRUE(r.isOk());

    // Balances should be unchanged
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 1000);

    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 0);
}

TEST_F(AccountBufferTest, AddTransaction_NegativeAmount_Error) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, -100, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 15);
    EXPECT_EQ(r.error().message, "Transfer amount must be non-negative");
}

TEST_F(AccountBufferTest, AddTransaction_NegativeFee_Error) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, 100, -10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 16);
    EXPECT_EQ(r.error().message, "Fee must be non-negative");
}

TEST_F(AccountBufferTest, AddTransaction_SourceAccountNotFound_Error) {
    auto r = buf.addTransaction(999, 2, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 17);
    EXPECT_EQ(r.error().message, "Source account not found");
}

TEST_F(AccountBufferTest, AddTransaction_InsufficientBalance_GenesisToken_Error) {
    // Account has 100, trying to transfer 100 with 10 fee = 110 total
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 18);
    EXPECT_EQ(r.error().message, "Insufficient balance for transfer and fee");
    
    // Destination account should not be created
    EXPECT_FALSE(buf.hasAccount(2));
}

TEST_F(AccountBufferTest, AddTransaction_InsufficientBalance_ForTransfer_Error) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Account has 50 of custom token, trying to transfer 100
    AccountBuffer::Account a;
    a.id = 1;
    a.wallet.mBalances[CUSTOM_TOKEN] = 50;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100; // Enough for fee
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.addTransaction(1, 2, CUSTOM_TOKEN, 100, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 19);
    EXPECT_EQ(r.error().message, "Insufficient balance for transfer");
    
    // Destination account should not be created
    EXPECT_FALSE(buf.hasAccount(2));
}

TEST_F(AccountBufferTest, AddTransaction_InsufficientBalance_ForFee_Error) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Account has enough custom token but not enough ID_GENESIS for fee
    AccountBuffer::Account a;
    a.id = 1;
    a.wallet.mBalances[CUSTOM_TOKEN] = 500;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 5; // Not enough for fee of 10
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.addTransaction(1, 2, CUSTOM_TOKEN, 100, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 20);
    EXPECT_EQ(r.error().message, "Insufficient balance for fee");
    
    // Destination account should not be created
    EXPECT_FALSE(buf.hasAccount(2));
}

TEST_F(AccountBufferTest, AddTransaction_ZeroFee_Success) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    // Transfer with zero fee
    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, 100, 0);
    ASSERT_TRUE(r.isOk());

    // Source: 1000 - 100 = 900 (no fee deducted)
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 900);

    // Destination: 100
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, AddTransaction_ExactBalance_Success) {
    // Account has exactly enough for transfer + fee
    auto a = makeAccount(1, 110);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.addTransaction(1, 2, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isOk());

    // Source: 110 - 100 - 10 = 0
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 0);

    // Destination: 100
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, AddTransaction_DestinationIsGenesis_NegativeBalanceAllowed) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    // Transfer to genesis account (ID_GENESIS)
    auto r = buf.addTransaction(1, AccountBuffer::ID_GENESIS, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isOk());

    auto genesis = buf.getAccount(AccountBuffer::ID_GENESIS);
    ASSERT_TRUE(genesis.isOk());
    EXPECT_EQ(genesis.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}
