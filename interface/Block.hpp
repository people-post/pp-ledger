#pragma once

#include <string>
#include <cstdint>

namespace pp {
namespace iii {

/**
 * Interface for Block data structure.
 * Implementations should provide concrete block representation.
 * 
 * Used by consensus to validate blocks and verify hash chains.
 */
class Block {
public:
    virtual ~Block() = default;
    
    // Core properties required by consensus and storage
    virtual uint64_t getIndex() const = 0;
    virtual int64_t getTimestamp() const = 0;
    virtual std::string getData() const = 0;
    virtual std::string getPreviousHash() const = 0;
    virtual std::string getHash() const = 0;
    virtual std::string calculateHash() const = 0;
    
    // Consensus-specific properties
    virtual uint64_t getSlot() const = 0;
    virtual std::string getSlotLeader() const = 0;
};

} // namespace iii
} // namespace pp
