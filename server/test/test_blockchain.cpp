#include "../Validator.h"
#include "../../ledger/Ledger.h"
#include <gtest/gtest.h>

class BlockChainTest : public ::testing::Test {
protected:
    pp::Validator::BlockChain* blockchain;
    
    void SetUp() override {
        blockchain = new pp::Validator::BlockChain();
    }
    
    void TearDown() override {
        delete blockchain;
    }
};

TEST_F(BlockChainTest, StartsEmpty) {
    // BlockChain doesn't auto-create a genesis block
    EXPECT_EQ(blockchain->getSize(), 0);
    auto latest = blockchain->getLatestBlock();
    EXPECT_EQ(latest, nullptr);
    EXPECT_EQ(blockchain->getLastBlockHash(), "0");
}

TEST_F(BlockChainTest, AddsBlocksToChain) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::Block>();
    block0->index = 0;
    block0->data = "Genesis block";
    block0->previousHash = "0";
    block0->hash = block0->calculateHash();
    blockchain->addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::Block>();
    block1->index = 1;
    block1->data = "Transaction 1: Alice -> Bob: 10 coins";
    block1->previousHash = blockchain->getLastBlockHash();
    block1->hash = block1->calculateHash();
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::Block>();
    block2->index = 2;
    block2->data = "Transaction 2: Bob -> Charlie: 5 coins";
    block2->previousHash = blockchain->getLastBlockHash();
    block2->hash = block2->calculateHash();
    blockchain->addBlock(block2);
    
    auto block3 = std::make_shared<pp::Ledger::Block>();
    block3->index = 3;
    block3->data = "Transaction 3: Charlie -> Alice: 3 coins";
    block3->previousHash = blockchain->getLastBlockHash();
    block3->hash = block3->calculateHash();
    blockchain->addBlock(block3);
    
    EXPECT_EQ(blockchain->getSize(), 4);
}

TEST_F(BlockChainTest, ValidatesCorrectChain) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::Block>();
    block0->index = 0;
    block0->data = "Genesis";
    block0->previousHash = "0";
    block0->hash = block0->calculateHash();
    blockchain->addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::Block>();
    block1->index = 1;
    block1->data = "Transaction 1";
    block1->previousHash = blockchain->getLastBlockHash();
    block1->hash = block1->calculateHash();
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::Block>();
    block2->index = 2;
    block2->data = "Transaction 2";
    block2->previousHash = blockchain->getLastBlockHash();
    block2->hash = block2->calculateHash();
    blockchain->addBlock(block2);
    
    EXPECT_TRUE(blockchain->isValid());
}

TEST_F(BlockChainTest, DetectsTampering) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::Block>();
    block0->index = 0;
    block0->data = "Genesis";
    block0->previousHash = "0";
    block0->hash = block0->calculateHash();
    blockchain->addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::Block>();
    block1->index = 1;
    block1->data = "Original Transaction";
    block1->previousHash = blockchain->getLastBlockHash();
    block1->hash = block1->calculateHash();
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::Block>();
    block2->index = 2;
    block2->data = "Another Transaction";
    block2->previousHash = blockchain->getLastBlockHash();
    block2->hash = block2->calculateHash();
    blockchain->addBlock(block2);
    
    // Verify chain is initially valid
    EXPECT_TRUE(blockchain->isValid());
    
    // Manually tamper with the chain - get block 1 and tamper with it
    auto blockPtr = blockchain->getBlock(1);
    if (blockPtr) {
        // Tamper by modifying hash without proper recalculation
        blockPtr->hash = "tampered";
    }
    
    EXPECT_FALSE(blockchain->isValid());
}

TEST_F(BlockChainTest, BlocksHaveCorrectIndices) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::Block>();
    block0->index = 0;
    block0->data = "Block 0";
    block0->previousHash = "0";
    block0->hash = block0->calculateHash();
    blockchain->addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::Block>();
    block1->index = 1;
    block1->data = "Block 1";
    block1->previousHash = blockchain->getLastBlockHash();
    block1->hash = block1->calculateHash();
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::Block>();
    block2->index = 2;
    block2->data = "Block 2";
    block2->previousHash = blockchain->getLastBlockHash();
    block2->hash = block2->calculateHash();
    blockchain->addBlock(block2);
    
    EXPECT_EQ(blockchain->getBlock(0)->index, 0);
    EXPECT_EQ(blockchain->getBlock(1)->index, 1);
    EXPECT_EQ(blockchain->getBlock(2)->index, 2);
}

TEST_F(BlockChainTest, BlocksLinkedByHash) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::Block>();
    block0->index = 0;
    block0->data = "Block 0";
    block0->previousHash = "0";
    block0->hash = block0->calculateHash();
    blockchain->addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::Block>();
    block1->index = 1;
    block1->data = "Block 1";
    block1->previousHash = blockchain->getLastBlockHash();
    block1->hash = block1->calculateHash();
    blockchain->addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::Block>();
    block2->index = 2;
    block2->data = "Block 2";
    block2->previousHash = blockchain->getLastBlockHash();
    block2->hash = block2->calculateHash();
    blockchain->addBlock(block2);
    
    EXPECT_EQ(blockchain->getBlock(1)->previousHash, blockchain->getBlock(0)->hash);
    EXPECT_EQ(blockchain->getBlock(2)->previousHash, blockchain->getBlock(1)->hash);
}

TEST_F(BlockChainTest, GetLatestBlock) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::Block>();
    block0->index = 0;
    block0->data = "Genesis";
    block0->previousHash = "0";
    block0->hash = block0->calculateHash();
    blockchain->addBlock(block0);
    
    auto block = std::make_shared<pp::Ledger::Block>();
    block->index = 1;
    block->data = "Latest Block";
    block->previousHash = blockchain->getLastBlockHash();
    block->hash = block->calculateHash();
    blockchain->addBlock(block);
    
    auto latest = blockchain->getLatestBlock();
    EXPECT_NE(latest, nullptr);
    EXPECT_EQ(latest->data, "Latest Block");
}
