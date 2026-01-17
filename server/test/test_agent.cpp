#include "Agent.h"
#include <gtest/gtest.h>

class AgentTest : public ::testing::Test {
protected:
    pp::Agent* agent;
    
    void SetUp() override {
        agent = new pp::Agent();
    }
    
    void TearDown() override {
        delete agent;
    }

    // Helper function to create a simple validator that always returns true
    static pp::Agent::Roe<bool> simpleValidator(const pp::iii::Block&, const pp::iii::BlockChain&) {
        return true;
    }
};

// Test blockchain operations (public methods)
TEST_F(AgentTest, ProducesBlockWithEmptyTransactionsFails) {
    // Try to produce a block with no pending transactions
    auto result = agent->produceBlock(0, "leader", simpleValidator);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 1); // Error code for no pending transactions
}

TEST_F(AgentTest, ValidatesEmptyBlockchain) {
    // Empty blockchain is invalid (BlockChain::isValid() returns false for empty chain)
    EXPECT_FALSE(agent->isValid());
}

TEST_F(AgentTest, GetBlockCountInitiallyZero) {
    // Agent starts with no blocks (no auto-genesis block)
    EXPECT_EQ(agent->getBlockCount(), 0);
}

TEST_F(AgentTest, GetLatestBlockInitiallyNull) {
    // Agent starts with no blocks
    auto block = agent->getLatestBlock();
    EXPECT_EQ(block, nullptr);
}

TEST_F(AgentTest, GetSizeInitiallyZero) {
    // Agent starts with no blocks
    EXPECT_EQ(agent->getSize(), 0);
}

// Test wallet query operations (public methods)
TEST_F(AgentTest, GetBalanceOfNonExistentWallet) {
    auto balance = agent->getBalance("NonExistent");
    EXPECT_TRUE(balance.isError());
}

TEST_F(AgentTest, HasWalletForNonExistentWallet) {
    EXPECT_FALSE(agent->hasWallet("NonExistent"));
}

// Test transfer operation (public method) - will fail if wallets don't exist
TEST_F(AgentTest, TransferRequiresWallets) {
    // Transfer will fail because wallets don't exist (can't create them publicly)
    pp::Agent::Transaction tx("Alice", "Bob", 100);
    auto result = agent->addTransaction(tx);
    EXPECT_TRUE(result.isError());
}
