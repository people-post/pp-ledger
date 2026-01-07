#pragma once

#include <string>
#include <cstdint>

namespace pp {
namespace consensus {

/**
 * Interface for Block data structure.
 * Implementations should provide concrete block representation.
 */
class IBlock {
public:
    virtual ~IBlock() = default;
    
    // Core block properties
    virtual uint64_t getIndex() const = 0;
    virtual int64_t getTimestamp() const = 0;
    virtual std::string getData() const = 0;
    virtual std::string getPreviousHash() const = 0;
    virtual std::string getHash() const = 0;
    virtual uint64_t getNonce() const = 0;
    
    // Block operations
    virtual std::string calculateHash() const = 0;
    virtual void setHash(const std::string& hash) = 0;
    virtual void setNonce(uint64_t nonce) = 0;
    
    // Ouroboros specific
    virtual uint64_t getSlot() const = 0;
    virtual std::string getSlotLeader() const = 0;
    virtual void setSlot(uint64_t slot) = 0;
    virtual void setSlotLeader(const std::string& leader) = 0;
};

} // namespace consensus
} // namespace pp
