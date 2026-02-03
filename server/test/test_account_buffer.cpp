#include "../AccountBuffer.h"
#include "../Validator.h"
#include <gtest/gtest.h>
#include <cstdint>

using namespace pp;

class AccountBufferTest : public ::testing::Test {
protected:
    AccountBuffer buf;

    AccountBuffer::Account makeAccount(uint64_t id, int64_t balance,
                                       bool isNegativeBalanceAllowed = false) {
        AccountBuffer::Account a;
        a.id = id;
        a.publicKeys = {"pk-" + std::to_string(id)};
        a.mBalances[AccountBuffer::ID_GENESIS] = balance; // Use native token (ID ID_GENESIS)
        a.isNegativeBalanceAllowed = isNegativeBalanceAllowed;
        return a;
    }
};

// --- depositBalance ---

TEST_F(AccountBufferTest, DepositBalance_Success_IncreasesBalance) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.depositBalance(1, AccountBuffer::ID_GENESIS, 50); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), 150);
}

TEST_F(AccountBufferTest, DepositBalance_ZeroAmount_Success) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.depositBalance(1, AccountBuffer::ID_GENESIS, 0); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), 100);
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

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), INT64_MAX);
}

// --- withdrawBalance ---

TEST_F(AccountBufferTest, WithdrawBalance_Success_DecreasesBalance) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 30); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), 70);
}

TEST_F(AccountBufferTest, WithdrawBalance_ZeroAmount_Success) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 0); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, WithdrawBalance_ExactBalance_Success) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), 0);
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
    a.isNegativeBalanceAllowed = false;
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 13);
    EXPECT_EQ(r.error().message, "Insufficient balance");

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), 50);
}

TEST_F(AccountBufferTest, WithdrawBalance_NegativeBalanceAllowed_Success) {
    auto a = makeAccount(1, 50);
    a.isNegativeBalanceAllowed = true;
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isOk());

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), -50);
}

TEST_F(AccountBufferTest, WithdrawBalance_Underflow_Error) {
    auto a = makeAccount(1, INT64_MIN);
    a.isNegativeBalanceAllowed = true;
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 1); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, 14);
    EXPECT_EQ(r.error().message, "Withdraw would cause balance underflow");

    auto got = buf.get(1);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().mBalances.at(AccountBuffer::ID_GENESIS), INT64_MIN);
}
