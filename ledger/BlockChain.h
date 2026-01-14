#pragma once

#include "Block.h"
#include "Module.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace pp {

/**
 * Concrete implementation of BlockChain data structure
 * 
 * Manages an in-memory chain of blocks.
 * Storage management is handled by Ledger.
 */
class BlockChain : public Module {
public:
    BlockChain();
    ~BlockChain() = default;
    
    // Blockchain operations
    std::shared_ptr<Block> getLatestBlock() const;
    size_t getSize() const;
    
    // Additional blockchain operations
    bool addBlock(std::shared_ptr<Block> block);
    std::shared_ptr<Block> getBlock(uint64_t index) const;
    bool isValid() const;
    std::string getLastBlockHash() const;
    
    /**
     * Trim blocks from the head of the chain
     * Removes the first n blocks from the beginning of the chain
     * @param count Number of blocks to trim from the head
     * @return Number of blocks removed
     */
    size_t trimBlocks(size_t count);
    
private:
    // Internal helper methods
    bool validateBlock(const Block& block) const;
    std::vector<std::shared_ptr<Block>> getBlocks(uint64_t fromIndex, uint64_t toIndex) const;
    
    std::vector<std::shared_ptr<Block>> chain_;
};

} // namespace pp
