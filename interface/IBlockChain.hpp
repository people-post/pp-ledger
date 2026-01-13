#pragma once

#include "IBlock.hpp"
#include <memory>
#include <vector>

namespace pp {
namespace consensus {

/**
 * Interface for BlockChain data structure.
 * Implementations should provide concrete blockchain representation.
 */
class IBlockChain {
public:
    virtual ~IBlockChain() = default;
    
    // Chain operations
    virtual bool addBlock(std::shared_ptr<IBlock> block) = 0;
    virtual std::shared_ptr<IBlock> getLatestBlock() const = 0;
    virtual std::shared_ptr<IBlock> getBlock(uint64_t index) const = 0;
    virtual size_t getSize() const = 0;
    
    // Validation
    virtual bool isValid() const = 0;
    virtual bool validateBlock(const IBlock& block) const = 0;
    
    // Query operations
    virtual std::vector<std::shared_ptr<IBlock>> getBlocks(uint64_t fromIndex, uint64_t toIndex) const = 0;
    virtual std::string getLastBlockHash() const = 0;
};

} // namespace consensus
} // namespace pp
