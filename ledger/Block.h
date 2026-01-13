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
    
    // IBlock interface implementation
    uint64_t getIndex() const override;
    int64_t getTimestamp() const override;
    std::string getData() const override;
    std::string getPreviousHash() const override;
    std::string getHash() const override;
    uint64_t getNonce() const override;
    
    std::string calculateHash() const override;
    void setHash(const std::string& hash) override;
    void setNonce(uint64_t nonce) override;
    
    // Additional setters
    void setIndex(uint64_t index);
    void setTimestamp(int64_t timestamp);
    void setData(const std::string& data);
    void setPreviousHash(const std::string& hash);
    
    // Additional operations
    void mineBlock(uint32_t difficulty);

private:
    uint64_t index_;
    int64_t timestamp_;
    std::string data_;
    std::string previousHash_;
    std::string hash_;
    uint64_t nonce_;
};

} // namespace pp
