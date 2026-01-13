#pragma once

#include "Block.hpp"
#include <memory>
#include <vector>

namespace pp {
namespace iii {

/**
 * Interface for BlockChain data structure.
 * Implementations should provide concrete blockchain representation.
 * 
 * Used by consensus to validate blocks and select between competing chains.
 */
class BlockChain {
public:
    virtual ~BlockChain() = default;
    
    // Chain state queries
    virtual std::shared_ptr<Block> getLatestBlock() const = 0;
    virtual size_t getSize() const = 0;
};

} // namespace iii
} // namespace pp
