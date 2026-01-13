#pragma once

#include "Block.hpp"
#include <memory>
#include <vector>

namespace pp {
namespace iii {

/**
 * Interface for BlockChain data structure.
 * Implementations should provide concrete blockchain representation.
 */
class BlockChain {
public:
    virtual ~BlockChain() = default;
    
    // Chain operations
    virtual bool addBlock(std::shared_ptr<Block> block) = 0;
    virtual std::shared_ptr<Block> getLatestBlock() const = 0;
    virtual std::shared_ptr<Block> getBlock(uint64_t index) const = 0;
    virtual size_t getSize() const = 0;
    
    // Validation
    virtual bool isValid() const = 0;
    virtual bool validateBlock(const Block& block) const = 0;
    
    // Query operations
    virtual std::vector<std::shared_ptr<Block>> getBlocks(uint64_t fromIndex, uint64_t toIndex) const = 0;
    virtual std::string getLastBlockHash() const = 0;
};

} // namespace iii
} // namespace pp
