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
    std::shared_ptr<IBlock> getLatestBlock() const;
    size_t getSize() const;
    
    // Additional blockchain operations
    bool addBlock(std::shared_ptr<IBlock> block);
    std::shared_ptr<IBlock> getBlock(uint64_t index) const;
    bool isValid() const;
    std::string getLastBlockHash() const;
    
    /**
     * Trim blocks from the chain
     * Removes blocks whose indices are in the provided set
     * @param blockIndices Set of block indices to remove
     * @return Number of blocks removed
     */
    size_t trimBlocks(const std::vector<uint64_t>& blockIndices);
    
private:
    void createGenesisBlock();
    
    // Internal helper methods
    bool validateBlock(const IBlock& block) const;
    std::vector<std::shared_ptr<IBlock>> getBlocks(uint64_t fromIndex, uint64_t toIndex) const;
    
    std::vector<std::shared_ptr<IBlock>> chain_;
};

} // namespace pp
