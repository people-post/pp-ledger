#include "BlockChain.h"
#include <gtest/gtest.h>

class BlockChainTest : public ::testing::Test {
protected:
    pp::BlockChain* blockchain;
    
    void SetUp() override {
        blockchain = new pp::BlockChain(2);
    }
    
    void TearDown() override {
        delete blockchain;
    }
};

TEST_F(BlockChainTest, CreatesWithGenesisBlock) {
    EXPECT_EQ(blockchain->getSize(), 1);
    const auto& genesis = blockchain->getBlock(0);
    EXPECT_EQ(genesis.index, 0);
    EXPECT_EQ(genesis.previousHash, "0");
}

TEST_F(BlockChainTest, AddsBlocksToChain) {
    blockchain->addBlock("Transaction 1: Alice -> Bob: 10 coins");
    blockchain->addBlock("Transaction 2: Bob -> Charlie: 5 coins");
    blockchain->addBlock("Transaction 3: Charlie -> Alice: 3 coins");
    
    EXPECT_EQ(blockchain->getSize(), 4);
}

TEST_F(BlockChainTest, ValidatesCorrectChain) {
    blockchain->addBlock("Transaction 1");
    blockchain->addBlock("Transaction 2");
    
    EXPECT_TRUE(blockchain->isValid());
}

TEST_F(BlockChainTest, DetectsTampering) {
    blockchain->addBlock("Original Transaction");
    blockchain->addBlock("Another Transaction");
    
    // Manually tamper with the chain
    auto& chain = const_cast<std::vector<pp::Block>&>(blockchain->getChain());
    chain[1].data = "Tampered Transaction!!!";
    
    EXPECT_FALSE(blockchain->isValid());
}

TEST_F(BlockChainTest, BlocksHaveCorrectIndices) {
    blockchain->addBlock("Block 1");
    blockchain->addBlock("Block 2");
    
    const auto& chain = blockchain->getChain();
    EXPECT_EQ(chain[0].index, 0);
    EXPECT_EQ(chain[1].index, 1);
    EXPECT_EQ(chain[2].index, 2);
}

TEST_F(BlockChainTest, BlocksLinkedByHash) {
    blockchain->addBlock("Block 1");
    blockchain->addBlock("Block 2");
    
    const auto& chain = blockchain->getChain();
    EXPECT_EQ(chain[1].previousHash, chain[0].hash);
    EXPECT_EQ(chain[2].previousHash, chain[1].hash);
}

TEST_F(BlockChainTest, GetLatestBlock) {
    blockchain->addBlock("Latest Block");
    const auto& latest = blockchain->getLatestBlock();
    EXPECT_EQ(latest.data, "Latest Block");
}
