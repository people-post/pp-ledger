#include "BlockChain.h"
#include "Block.h"

#include <stdexcept>
#include <algorithm>
#include <unordered_set>
#include <openssl/evp.h>

namespace pp {

// BlockChain implementation
BlockChain::BlockChain()
    : Module("blockchain") {
    createGenesisBlock();
}

void BlockChain::createGenesisBlock() {
    auto genesis = std::make_shared<Block>();
    genesis->setIndex(0);
    genesis->setData("Genesis Block");
    genesis->setPreviousHash("0");
    genesis->setHash(genesis->calculateHash());
    chain_.push_back(genesis);
}

// Blockchain operations
bool BlockChain::addBlock(std::shared_ptr<IBlock> block) {
    if (!block) {
        return false;
    }
    
    chain_.push_back(block);
    return true;
}

std::shared_ptr<IBlock> BlockChain::getLatestBlock() const {
    if (chain_.empty()) {
        return nullptr;
    }
    return chain_.back();
}

std::shared_ptr<IBlock> BlockChain::getBlock(uint64_t index) const {
    if (index >= chain_.size()) {
        return nullptr;
    }
    return chain_[index];
}

size_t BlockChain::getSize() const {
    return chain_.size();
}

bool BlockChain::isValid() const {
    if (chain_.empty()) {
        return false;
    }
    
    // Validate all blocks including genesis
    for (size_t i = 0; i < chain_.size(); i++) {
        const auto& currentBlock = chain_[i];
        
        // Verify current block's hash
        if (currentBlock->getHash() != currentBlock->calculateHash()) {
            return false;
        }
        
        // Verify link to previous block (skip for first block if it has special previousHash "0")
        if (i > 0) {
            const auto& previousBlock = chain_[i - 1];
            if (currentBlock->getPreviousHash() != previousBlock->getHash()) {
                return false;
            }
        }
    }
    
    return true;
}

bool BlockChain::validateBlock(const IBlock& block) const {
    // Verify block's hash
    if (block.getHash() != block.calculateHash()) {
        return false;
    }
    
    return true;
}

std::vector<std::shared_ptr<IBlock>> BlockChain::getBlocks(uint64_t fromIndex, uint64_t toIndex) const {
    std::vector<std::shared_ptr<IBlock>> result;
    
    if (fromIndex > toIndex || fromIndex >= chain_.size()) {
        return result;
    }
    
    uint64_t endIndex = std::min(toIndex + 1, static_cast<uint64_t>(chain_.size()));
    for (uint64_t i = fromIndex; i < endIndex; i++) {
        result.push_back(chain_[i]);
    }
    
    return result;
}

std::string BlockChain::getLastBlockHash() const {
    if (chain_.empty()) {
        return "0";
    }
    return chain_.back()->getHash();
}

size_t BlockChain::trimBlocks(const std::vector<uint64_t>& blockIndices) {
    if (blockIndices.empty() || chain_.empty()) {
        return 0; // Nothing to trim or empty chain
    }
    
    // Create a set for fast lookup
    std::unordered_set<uint64_t> indicesToRemove(blockIndices.begin(), blockIndices.end());
    
    size_t removed = 0;
    // Remove blocks whose indices are in the set
    // Iterate backwards to avoid index shifting issues
    for (int64_t i = static_cast<int64_t>(chain_.size()) - 1; i >= 0; i--) {
        uint64_t blockIndex = chain_[i]->getIndex();
        if (indicesToRemove.find(blockIndex) != indicesToRemove.end()) {
            chain_.erase(chain_.begin() + i);
            removed++;
        }
    }
    
    return removed;
}

} // namespace pp
