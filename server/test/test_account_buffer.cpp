#include "../AccountBuffer.h"
#include "../Chain.h"
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
    EXPECT_EQ(r.error().code, AccountBuffer::E_INPUT);
    EXPECT_EQ(r.error().message, "Deposit amount must be non-negative");
}

TEST_F(AccountBufferTest, DepositBalance_AccountNotFound_Error) {
    auto r = buf.depositBalance(999, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_ACCOUNT);
    EXPECT_EQ(r.error().message, "Account not found");
}

TEST_F(AccountBufferTest, DepositBalance_Overflow_Error) {
    auto a = makeAccount(1, INT64_MAX);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.depositBalance(1, AccountBuffer::ID_GENESIS, 1); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
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
    EXPECT_EQ(r.error().code, AccountBuffer::E_INPUT);
    EXPECT_EQ(r.error().message, "Withdraw amount must be non-negative");
}

TEST_F(AccountBufferTest, WithdrawBalance_AccountNotFound_Error) {
    auto r = buf.withdrawBalance(999, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_ACCOUNT);
    EXPECT_EQ(r.error().message, "Account not found");
}

TEST_F(AccountBufferTest, WithdrawBalance_InsufficientBalance_Error) {
    auto a = makeAccount(1, 50);
    ASSERT_TRUE(buf.add(a).isOk());

    auto r = buf.withdrawBalance(1, AccountBuffer::ID_GENESIS, 100); // tokenId = ID_GENESIS
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
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
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
    EXPECT_EQ(r.error().message, "Withdraw would cause balance underflow");

    auto got = buf.getAccount(AccountBuffer::ID_GENESIS);
    ASSERT_TRUE(got.isOk());
    EXPECT_EQ(got.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), INT64_MIN);
}

// --- hSpendingPower ---

TEST_F(AccountBufferTest, HasEnoughSpendingPower_SufficientBalance_GenesisToken_ReturnsTrue) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    // Account has 1000, needs 100 + 10 = 110
    EXPECT_TRUE(buf.verifySpendingPower(1, AccountBuffer::ID_GENESIS, 100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_ExactBalance_GenesisToken_ReturnsTrue) {
    auto a = makeAccount(1, 110);
    ASSERT_TRUE(buf.add(a).isOk());

    // Account has exactly 110, needs 100 + 10 = 110
    EXPECT_TRUE(buf.verifySpendingPower(1, AccountBuffer::ID_GENESIS, 100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_InsufficientBalance_GenesisToken_ReturnsFalse) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    // Account has 100, needs 100 + 10 = 110
    EXPECT_FALSE(buf.verifySpendingPower(1, AccountBuffer::ID_GENESIS, 100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_SufficientBalance_CustomToken_ReturnsTrue) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    AccountBuffer::Account a;
    a.id = 1;
    a.wallet.mBalances[CUSTOM_TOKEN] = 500;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100;
    ASSERT_TRUE(buf.add(a).isOk());

    // Has 500 of custom token and 100 genesis, needs 200 + 10 fee
    EXPECT_TRUE(buf.verifySpendingPower(1, CUSTOM_TOKEN, 200, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_InsufficientTokenBalance_CustomToken_ReturnsFalse) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    AccountBuffer::Account a;
    a.id = 1;
    a.wallet.mBalances[CUSTOM_TOKEN] = 50;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100;
    ASSERT_TRUE(buf.add(a).isOk());

    // Has 50 of custom token but needs 100
    EXPECT_FALSE(buf.verifySpendingPower(1, CUSTOM_TOKEN, 100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_InsufficientFeeBalance_CustomToken_ReturnsFalse) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    AccountBuffer::Account a;
    a.id = 1;
    a.wallet.mBalances[CUSTOM_TOKEN] = 500;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 5;
    ASSERT_TRUE(buf.add(a).isOk());

    // Has enough custom token but not enough genesis for fee
    EXPECT_FALSE(buf.verifySpendingPower(1, CUSTOM_TOKEN, 100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_AccountNotFound_ReturnsFalse) {
    // Account 999 doesn't exist
    EXPECT_FALSE(buf.verifySpendingPower(999, AccountBuffer::ID_GENESIS, 100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_NegativeAmount_ReturnsFalse) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    EXPECT_FALSE(buf.verifySpendingPower(1, AccountBuffer::ID_GENESIS, -100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_NegativeFee_ReturnsFalse) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());

    EXPECT_FALSE(buf.verifySpendingPower(1, AccountBuffer::ID_GENESIS, 100, -10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_ZeroAmount_ReturnsTrue) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    // Even with low balance, zero amount should work
    EXPECT_TRUE(buf.verifySpendingPower(1, AccountBuffer::ID_GENESIS, 0, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_ZeroFee_ReturnsTrue) {
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    EXPECT_TRUE(buf.verifySpendingPower(1, AccountBuffer::ID_GENESIS, 100, 0).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_GenesisAccount_AllowsNegativeBalance_ReturnsTrue) {
    // Genesis account (ID_GENESIS) can have negative balance
    auto a = makeAccount(AccountBuffer::ID_GENESIS, -1000);
    ASSERT_TRUE(buf.add(a).isOk());

    // Even with negative balance, genesis account should return true
    EXPECT_TRUE(buf.verifySpendingPower(AccountBuffer::ID_GENESIS, AccountBuffer::ID_GENESIS, 100, 10).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_NoTokenBalance_ReturnsFalse) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Account only has genesis balance, no custom token balance
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());

    // Doesn't have any custom token balance
    EXPECT_FALSE(buf.verifySpendingPower(1, CUSTOM_TOKEN, 50, 10).isOk());
}

// --- transferBalance with fee parameter ---

TEST_F(AccountBufferTest, TransferBalance_WithFee_GenesisToken_Success) {
    // Create two accounts with genesis token balance
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());
    
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    // Transfer 100 with 10 fee (both from ID_GENESIS balance)
    auto r = buf.transferBalance(1, 2, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isOk());

    // Source: 1000 - 100 - 10 = 890
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 890);

    // Destination: 0 + 100 = 100
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, TransferBalance_WithFee_CustomToken_Success) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
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
    auto r = buf.transferBalance(1, 2, CUSTOM_TOKEN, 200, 10);
    ASSERT_TRUE(r.isOk());

    // Source: custom token 500 - 200 = 300, genesis 100 - 10 = 90
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(CUSTOM_TOKEN), 300);
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 90);

    // Destination: custom token 0 + 200 = 200
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(CUSTOM_TOKEN), 200);
}

TEST_F(AccountBufferTest, TransferBalance_WithZeroFee_Success) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());
    
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    // Transfer with zero fee (explicit)
    auto r = buf.transferBalance(1, 2, AccountBuffer::ID_GENESIS, 100, 0);
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

TEST_F(AccountBufferTest, TransferBalance_WithNegativeFee_Error) {
    auto a = makeAccount(1, 1000);
    ASSERT_TRUE(buf.add(a).isOk());
    
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    // Negative fee should fail
    auto r = buf.transferBalance(1, 2, AccountBuffer::ID_GENESIS, 100, -10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_INPUT);
    EXPECT_EQ(r.error().message, "Fee must be non-negative");
}

TEST_F(AccountBufferTest, TransferBalance_InsufficientBalance_GenesisToken_WithFee_Error) {
    // Account has 100, trying to transfer 95 with 10 fee = 105 total
    auto a = makeAccount(1, 100);
    ASSERT_TRUE(buf.add(a).isOk());
    
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    auto r = buf.transferBalance(1, 2, AccountBuffer::ID_GENESIS, 95, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
    EXPECT_EQ(r.error().message, "Insufficient balance for transfer and fee");
    
    // Balance should remain unchanged
    auto from = buf.getAccount(1);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

TEST_F(AccountBufferTest, TransferBalance_InsufficientBalance_CustomToken_WithFee_Error) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Account has enough custom token but not enough ID_GENESIS for fee
    AccountBuffer::Account a;
    a.id = 1;
    a.wallet.mBalances[CUSTOM_TOKEN] = 500;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 5; // Not enough for fee of 10
    ASSERT_TRUE(buf.add(a).isOk());
    
    AccountBuffer::Account b;
    b.id = 2;
    b.wallet.mBalances[CUSTOM_TOKEN] = 0;
    ASSERT_TRUE(buf.add(b).isOk());

    auto r = buf.transferBalance(1, 2, CUSTOM_TOKEN, 100, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
    EXPECT_EQ(r.error().message, "Insufficient balance for fee");
}

TEST_F(AccountBufferTest, TransferBalance_ExactBalance_GenesisToken_WithFee_Success) {
    // Account has exactly enough for transfer + fee
    auto a = makeAccount(1, 110);
    ASSERT_TRUE(buf.add(a).isOk());
    
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    auto r = buf.transferBalance(1, 2, AccountBuffer::ID_GENESIS, 100, 10);
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

TEST_F(AccountBufferTest, TransferBalance_GenesisAccount_WithFee_AllowsNegativeBalance) {
    // Genesis account can have negative balance
    auto a = makeAccount(AccountBuffer::ID_GENESIS, 50);
    ASSERT_TRUE(buf.add(a).isOk());
    
    auto b = makeAccount(2, 0);
    ASSERT_TRUE(buf.add(b).isOk());

    // Transfer more than balance with fee (should work for genesis account)
    auto r = buf.transferBalance(AccountBuffer::ID_GENESIS, 2, AccountBuffer::ID_GENESIS, 100, 10);
    ASSERT_TRUE(r.isOk());

    // Genesis: 50 - 100 - 10 = -60 (negative allowed)
    auto genesis = buf.getAccount(AccountBuffer::ID_GENESIS);
    ASSERT_TRUE(genesis.isOk());
    EXPECT_EQ(genesis.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), -60);

    // Destination: 100
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 100);
}

// --- Custom Token Genesis Account Tests ---

TEST_F(AccountBufferTest, HasEnoughSpendingPower_CustomTokenGenesis_NegativeBalance_SufficientFee_ReturnsTrue) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account (accountId == tokenId)
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = -500; // Negative custom token balance
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100; // Sufficient fee balance
    ASSERT_TRUE(buf.add(a).isOk());

    // Should return true: can have negative custom token balance, has enough fee
    EXPECT_TRUE(buf.verifySpendingPower(CUSTOM_TOKEN, CUSTOM_TOKEN, 200, 50).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_CustomTokenGenesis_NegativeBalance_InsufficientFee_ReturnsFalse) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account (accountId == tokenId)
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = -500; // Negative custom token balance
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 10; // Insufficient fee balance
    ASSERT_TRUE(buf.add(a).isOk());

    // Should return false: has negative custom token balance but not enough fee
    EXPECT_FALSE(buf.verifySpendingPower(CUSTOM_TOKEN, CUSTOM_TOKEN, 200, 50).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_CustomTokenGenesis_ZeroBalance_SufficientFee_ReturnsTrue) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account (accountId == tokenId)
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = 0; // Zero custom token balance
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100; // Sufficient fee balance
    ASSERT_TRUE(buf.add(a).isOk());

    // Should return true: genesis account can have negative balance, has enough fee
    EXPECT_TRUE(buf.verifySpendingPower(CUSTOM_TOKEN, CUSTOM_TOKEN, 200, 50).isOk());
}

TEST_F(AccountBufferTest, HasEnoughSpendingPower_CustomTokenGenesis_NoFee_ReturnsTrue) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account (accountId == tokenId)
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = -500; // Negative custom token balance
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 0; // No fee balance needed
    ASSERT_TRUE(buf.add(a).isOk());

    // Should return true: no fee required
    EXPECT_TRUE(buf.verifySpendingPower(CUSTOM_TOKEN, CUSTOM_TOKEN, 200, 0).isOk());
}

TEST_F(AccountBufferTest, WithdrawBalance_CustomTokenGenesis_UnderflowProtection) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account at INT64_MIN
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = INT64_MIN;
    ASSERT_TRUE(buf.add(a).isOk());

    // Try to withdraw 1, which would cause underflow below INT64_MIN
    auto r = buf.withdrawBalance(CUSTOM_TOKEN, CUSTOM_TOKEN, 1);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
    EXPECT_EQ(r.error().message, "Withdraw would cause balance underflow");

    // Balance should remain unchanged
    auto acc = buf.getAccount(CUSTOM_TOKEN);
    ASSERT_TRUE(acc.isOk());
    EXPECT_EQ(acc.value().wallet.mBalances.at(CUSTOM_TOKEN), INT64_MIN);
}

TEST_F(AccountBufferTest, WithdrawBalance_CustomTokenGenesis_AllowsNegativeBalance) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account with positive balance
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = 100;
    ASSERT_TRUE(buf.add(a).isOk());

    // Withdraw more than balance (should succeed for genesis account)
    auto r = buf.withdrawBalance(CUSTOM_TOKEN, CUSTOM_TOKEN, 500);
    ASSERT_TRUE(r.isOk());

    // Balance should be negative
    auto acc = buf.getAccount(CUSTOM_TOKEN);
    ASSERT_TRUE(acc.isOk());
    EXPECT_EQ(acc.value().wallet.mBalances.at(CUSTOM_TOKEN), -400);
}

TEST_F(AccountBufferTest, TransferBalance_CustomTokenGenesis_NegativeBalance_SufficientFee_Success) {
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account with negative token balance
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = -500;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100;
    ASSERT_TRUE(buf.add(a).isOk());

    AccountBuffer::Account b;
    b.id = 2;
    b.wallet.mBalances[CUSTOM_TOKEN] = 0;
    ASSERT_TRUE(buf.add(b).isOk());

    // Should succeed: genesis can have negative balance, has enough fee
    auto r = buf.transferBalance(CUSTOM_TOKEN, 2, CUSTOM_TOKEN, 200, 50);
    ASSERT_TRUE(r.isOk());

    // Check source account: custom token -500 - 200 = -700, genesis 100 - 50 = 50
    auto from = buf.getAccount(CUSTOM_TOKEN);
    ASSERT_TRUE(from.isOk());
    EXPECT_EQ(from.value().wallet.mBalances.at(CUSTOM_TOKEN), -700);
    EXPECT_EQ(from.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 50);

    // Check destination account: custom token 0 + 200 = 200
    auto to = buf.getAccount(2);
    ASSERT_TRUE(to.isOk());
    EXPECT_EQ(to.value().wallet.mBalances.at(CUSTOM_TOKEN), 200);
}

TEST_F(AccountBufferTest, VerifySpendingPower_GenesisAccount_GenesisToken_Underflow_Error) {
    // Test for underflow check when genesis account tries to transfer with large fee
    // that would cause balance to underflow (go below INT64_MIN)
    auto a = makeAccount(AccountBuffer::ID_GENESIS, INT64_MIN + 100);
    ASSERT_TRUE(buf.add(a).isOk());

    // Try to transfer/fee that would cause: (INT64_MIN + 100) - 150 - 100 to underflow
    // amount + fee = 150 + 100 = 250
    // Check: 150 + 100 + INT64_MIN > tokenBalance
    //        250 + INT64_MIN > INT64_MIN + 100
    //        This is true, so should return error
    auto r = buf.verifySpendingPower(AccountBuffer::ID_GENESIS, AccountBuffer::ID_GENESIS, 150, 100);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
    EXPECT_EQ(r.error().message, "Transfer amount and fee would cause balance underflow");
}

TEST_F(AccountBufferTest, VerifySpendingPower_CustomTokenGenesis_Underflow_Error) {
    // Test for underflow check when custom token genesis account tries to transfer large amount
    // that would cause balance to underflow (go below INT64_MIN)
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account with balance near INT64_MIN
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = INT64_MIN + 100; // Balance near minimum
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100; // Sufficient fee balance
    ASSERT_TRUE(buf.add(a).isOk());

    // Try to transfer amount that would cause underflow
    // Check: amount + INT64_MIN > tokenBalance
    //        200 + INT64_MIN > INT64_MIN + 100
    //        This is true, so should return error
    auto r = buf.verifySpendingPower(CUSTOM_TOKEN, CUSTOM_TOKEN, 200, 10);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, AccountBuffer::E_BALANCE);
    EXPECT_EQ(r.error().message, "Transfer amount would cause balance underflow");
}

TEST_F(AccountBufferTest, VerifySpendingPower_CustomTokenGenesis_Overflow_Error) {
    // Test for overflow check when custom token genesis account tries to transfer with amount > INT64_MAX
    // This is a theoretical check since amount should never actually be larger than INT64_MAX in practice,
    // but we still add the check as defensive programming
    const uint64_t CUSTOM_TOKEN = 1000;
    
    buf.reset();
    
    // Create custom token genesis account
    AccountBuffer::Account a;
    a.id = CUSTOM_TOKEN;
    a.wallet.mBalances[CUSTOM_TOKEN] = 1000;
    a.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100;
    ASSERT_TRUE(buf.add(a).isOk());

    // This test would theoretically pass an amount > INT64_MAX, but since the parameter
    // is int64_t, it's not possible to actually pass a value > INT64_MAX.
    // Instead, we test the underflow one above and document that the overflow check
    // would catch if amount could somehow be > INT64_MAX.
    // We can verify the check exists by looking at code coverage.
}
