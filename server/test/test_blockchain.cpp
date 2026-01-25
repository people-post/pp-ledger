#include "../Validator.h"
#include "../../ledger/Ledger.h"
#include "../../lib/Utilities.h"
#include <gtest/gtest.h>
#include <sstream>

class BlockChainTest : public ::testing::Test {
protected:
    pp::Validator::BlockChain blockchain;
    
    void SetUp() override {
        // BlockChain is now standalone, no parent needed
    }
    
    void TearDown() override {
        // No cleanup needed
    }
    
    // Test helper: calculate hash manually for testing
    std::string calculateTestHash(const pp::Ledger::Block& block) {
        std::stringstream ss;
        ss << pp::Ledger::Block::CURRENT_VERSION << block.index << block.timestamp 
           << block.data << block.previousHash << block.nonce;
        return pp::utl::sha256(ss.str());
    }
};

TEST_F(BlockChainTest, StartsEmpty) {
    // BlockChain doesn't auto-create a genesis block
    EXPECT_EQ(blockchain.getSize(), 0);
    auto latest = blockchain.getLatestBlock();
    EXPECT_EQ(latest, nullptr);
    EXPECT_EQ(blockchain.getLastBlockHash(), "0");
}

TEST_F(BlockChainTest, AddsBlocksToChain) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::ChainNode>();
    block0->block.index = 0;
    block0->block.data = "Genesis block";
    block0->block.previousHash = "0";
    block0->hash = calculateTestHash(block0->block);
    blockchain.addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::ChainNode>();
    block1->block.index = 1;
    block1->block.data = "Transaction 1: Alice -> Bob: 10 coins";
    block1->block.previousHash = blockchain.getLastBlockHash();
    block1->hash = calculateTestHash(block1->block);
    blockchain.addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::ChainNode>();
    block2->block.index = 2;
    block2->block.data = "Transaction 2: Bob -> Charlie: 5 coins";
    block2->block.previousHash = blockchain.getLastBlockHash();
    block2->hash = calculateTestHash(block2->block);
    blockchain.addBlock(block2);
    
    auto block3 = std::make_shared<pp::Ledger::ChainNode>();
    block3->block.index = 3;
    block3->block.data = "Transaction 3: Charlie -> Alice: 3 coins";
    block3->block.previousHash = blockchain.getLastBlockHash();
    block3->hash = calculateTestHash(block3->block);
    blockchain.addBlock(block3);
    
    EXPECT_EQ(blockchain.getSize(), 4);
}

TEST_F(BlockChainTest, BlocksHaveCorrectIndices) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::ChainNode>();
    block0->block.index = 0;
    block0->block.data = "Block 0";
    block0->block.previousHash = "0";
    block0->hash = calculateTestHash(block0->block);
    blockchain.addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::ChainNode>();
    block1->block.index = 1;
    block1->block.data = "Block 1";
    block1->block.previousHash = blockchain.getLastBlockHash();
    block1->hash = calculateTestHash(block1->block);
    blockchain.addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::ChainNode>();
    block2->block.index = 2;
    block2->block.data = "Block 2";
    block2->block.previousHash = blockchain.getLastBlockHash();
    block2->hash = calculateTestHash(block2->block);
    blockchain.addBlock(block2);
    
    EXPECT_EQ(blockchain.getBlock(0)->block.index, 0);
    EXPECT_EQ(blockchain.getBlock(1)->block.index, 1);
    EXPECT_EQ(blockchain.getBlock(2)->block.index, 2);
}

TEST_F(BlockChainTest, BlocksLinkedByHash) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::ChainNode>();
    block0->block.index = 0;
    block0->block.data = "Block 0";
    block0->block.previousHash = "0";
    block0->hash = calculateTestHash(block0->block);
    blockchain.addBlock(block0);
    
    auto block1 = std::make_shared<pp::Ledger::ChainNode>();
    block1->block.index = 1;
    block1->block.data = "Block 1";
    block1->block.previousHash = blockchain.getLastBlockHash();
    block1->hash = calculateTestHash(block1->block);
    blockchain.addBlock(block1);
    
    auto block2 = std::make_shared<pp::Ledger::ChainNode>();
    block2->block.index = 2;
    block2->block.data = "Block 2";
    block2->block.previousHash = blockchain.getLastBlockHash();
    block2->hash = calculateTestHash(block2->block);
    blockchain.addBlock(block2);
    
    EXPECT_EQ(blockchain.getBlock(1)->block.previousHash, blockchain.getBlock(0)->hash);
    EXPECT_EQ(blockchain.getBlock(2)->block.previousHash, blockchain.getBlock(1)->hash);
}

TEST_F(BlockChainTest, GetLatestBlock) {
    // Create first block (genesis)
    auto block0 = std::make_shared<pp::Ledger::ChainNode>();
    block0->block.index = 0;
    block0->block.data = "Genesis";
    block0->block.previousHash = "0";
    block0->hash = calculateTestHash(block0->block);
    blockchain.addBlock(block0);
    
    auto block = std::make_shared<pp::Ledger::ChainNode>();
    block->block.index = 1;
    block->block.data = "Latest Block";
    block->block.previousHash = blockchain.getLastBlockHash();
    block->hash = calculateTestHash(block->block);
    blockchain.addBlock(block);
    
    auto latest = blockchain.getLatestBlock();
    EXPECT_NE(latest, nullptr);
    EXPECT_EQ(latest->block.data, "Latest Block");
}
