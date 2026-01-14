#pragma once

#include "../interface/Block.hpp"
#include <string>
#include <cstdint>
#include <memory>

namespace pp {

// Using declaration for interface type
using IBlock = iii::Block;

/**
 * Concrete implementation of Block interface
 */
class Block : public IBlock {
public:
    Block();
    
    // IBlock interface implementation (consensus methods)
    uint64_t getIndex() const override;
    int64_t getTimestamp() const override;
    std::string getData() const override;
    std::string getPreviousHash() const override;
    std::string getHash() const override;
    std::string calculateHash() const override;
    uint64_t getSlot() const override;
    std::string getSlotLeader() const override;
    
    // Additional methods (not part of interface, but used by Ledger/tests)
    uint64_t getNonce() const;
    void setHash(const std::string& hash);
    void setNonce(uint64_t nonce);
    
    // Version control for future extension
    uint16_t getVersion() const;
    
    // Long-term support serialization for disk persistence
    /**
     * Serialize block to binary format for long-term storage
     * Format is version-aware and compact for efficient disk storage
     * Binary format: [version][index][timestamp][data_size+data][prevHash_size+prevHash][hash_size+hash][nonce][slot][leader_size+leader]
     * @return Serialized binary string representation
     */
    std::string ltsToString() const;
    
    /**
     * Deserialize block from binary format
     * @param str Serialized binary string representation
     * @return true if successful, false on error
     */
    bool ltsFromString(const std::string& str);
    
    // Additional setters
    void setIndex(uint64_t index);
    void setTimestamp(int64_t timestamp);
    void setData(const std::string& data);
    void setPreviousHash(const std::string& hash);
    void setSlot(uint64_t slot);
    void setSlotLeader(const std::string& leader);
    
private:
    static constexpr uint16_t CURRENT_VERSION = 1;
    
    uint64_t index_;
    int64_t timestamp_;
    std::string data_;
    std::string previousHash_;
    std::string hash_;
    uint64_t nonce_;
    uint64_t slot_;
    std::string slotLeader_;
};

} // namespace pp
