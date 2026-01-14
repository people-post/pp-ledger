#include "BlockChain.h"
#include <gtest/gtest.h>

class BlockChainTest : public ::testing::Test {
protected:
    pp::BlockChain* blockchain;
    
    void SetUp() override {
        blockchain = new pp::BlockChain();
    }
    
    void TearDown() override {
        delete blockchain;
    }
};

TEST_F(BlockChainTest, CreatesWithGenesisBlock) {
    EXPECT_EQ(blockchain->getSize(), 1);
    auto genesis = blockchain->getBlock(0);
    EXPECT_NE(genesis, nullptr);
    EXPECT_EQ(genesis->getIndex(), 0);
    EXPECT_EQ(genesis->getPreviousHash(), "0");
}

TEST_F(BlockChainTest, AddsBlocksToChain) {
    auto block1 = std::make_shared<pp::Block>();
    block1->setIndex(1);
    block1->setData("Transaction 1: Alice -> Bob: 10 coins");
    block1->setPreviousHash(blockchain->getLastBlockHash());
    block1->setHash(block1->calculateHash());
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Block>();
    block2->setIndex(2);
    block2->setData("Transaction 2: Bob -> Charlie: 5 coins");
    block2->setPreviousHash(blockchain->getLastBlockHash());
    block2->setHash(block2->calculateHash());
    blockchain->addBlock(block2);
    
    auto block3 = std::make_shared<pp::Block>();
    block3->setIndex(3);
    block3->setData("Transaction 3: Charlie -> Alice: 3 coins");
    block3->setPreviousHash(blockchain->getLastBlockHash());
    block3->setHash(block3->calculateHash());
    blockchain->addBlock(block3);
    
    EXPECT_EQ(blockchain->getSize(), 4);
}

TEST_F(BlockChainTest, ValidatesCorrectChain) {
    auto block1 = std::make_shared<pp::Block>();
    block1->setIndex(1);
    block1->setData("Transaction 1");
    block1->setPreviousHash(blockchain->getLastBlockHash());
    block1->setHash(block1->calculateHash());
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Block>();
    block2->setIndex(2);
    block2->setData("Transaction 2");
    block2->setPreviousHash(blockchain->getLastBlockHash());
    block2->setHash(block2->calculateHash());
    blockchain->addBlock(block2);
    
    EXPECT_TRUE(blockchain->isValid());
}

TEST_F(BlockChainTest, DetectsTampering) {
    auto block1 = std::make_shared<pp::Block>();
    block1->setIndex(1);
    block1->setData("Original Transaction");
    block1->setPreviousHash(blockchain->getLastBlockHash());
    block1->setHash(block1->calculateHash());
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Block>();
    block2->setIndex(2);
    block2->setData("Another Transaction");
    block2->setPreviousHash(blockchain->getLastBlockHash());
    block2->setHash(block2->calculateHash());
    blockchain->addBlock(block2);
    
    // Verify chain is initially valid
    EXPECT_TRUE(blockchain->isValid());
    
    // Manually tamper with the chain - get block 1 and tamper with it
    auto blockPtr = std::dynamic_pointer_cast<pp::Block>(blockchain->getBlock(1));
    if (blockPtr) {
        // Tamper by modifying hash without proper recalculation
        blockPtr->setHash("tampered");
    }
    
    EXPECT_FALSE(blockchain->isValid());
}

TEST_F(BlockChainTest, BlocksHaveCorrectIndices) {
    auto block1 = std::make_shared<pp::Block>();
    block1->setIndex(1);
    block1->setData("Block 1");
    block1->setPreviousHash(blockchain->getLastBlockHash());
    block1->setHash(block1->calculateHash());
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Block>();
    block2->setIndex(2);
    block2->setData("Block 2");
    block2->setPreviousHash(blockchain->getLastBlockHash());
    block2->setHash(block2->calculateHash());
    blockchain->addBlock(block2);
    
    EXPECT_EQ(blockchain->getBlock(0)->getIndex(), 0);
    EXPECT_EQ(blockchain->getBlock(1)->getIndex(), 1);
    EXPECT_EQ(blockchain->getBlock(2)->getIndex(), 2);
}

TEST_F(BlockChainTest, BlocksLinkedByHash) {
    auto block1 = std::make_shared<pp::Block>();
    block1->setIndex(1);
    block1->setData("Block 1");
    block1->setPreviousHash(blockchain->getLastBlockHash());
    block1->setHash(block1->calculateHash());
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Block>();
    block2->setIndex(2);
    block2->setData("Block 2");
    block2->setPreviousHash(blockchain->getLastBlockHash());
    block2->setHash(block2->calculateHash());
    blockchain->addBlock(block2);
    
    EXPECT_EQ(blockchain->getBlock(1)->getPreviousHash(), blockchain->getBlock(0)->getHash());
    EXPECT_EQ(blockchain->getBlock(2)->getPreviousHash(), blockchain->getBlock(1)->getHash());
}

TEST_F(BlockChainTest, GetLatestBlock) {
    auto block = std::make_shared<pp::Block>();
    block->setIndex(1);
    block->setData("Latest Block");
    block->setPreviousHash(blockchain->getLastBlockHash());
    block->setHash(block->calculateHash());
    blockchain->addBlock(block);
    
    auto latest = blockchain->getLatestBlock();
    EXPECT_NE(latest, nullptr);
    EXPECT_EQ(latest->getData(), "Latest Block");
}
