#pragma once

#include "Module.h"
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace pp {

struct Block {
    uint64_t index;
    int64_t timestamp;
    std::string data;
    std::string previousHash;
    std::string hash;
    uint64_t nonce;
    
    Block(uint64_t idx, const std::string& blockData, const std::string& prevHash);
    
    std::string calculateHash() const;
    void mineBlock(uint32_t difficulty);
};

class BlockChain : public Module {
public:
    BlockChain(uint32_t difficulty = 2);
    ~BlockChain() = default;
    
    // Blockchain operations
    void addBlock(const std::string& data);
    const std::vector<Block>& getChain() const;
    bool isValid() const;
    
    // Query operations
    size_t getSize() const;
    const Block& getLatestBlock() const;
    const Block& getBlock(size_t index) const;
    
    // Configuration
    void setDifficulty(uint32_t difficulty);
    uint32_t getDifficulty() const;
    
private:
    void createGenesisBlock();
    std::string getLastBlockHash() const;
    
    std::vector<Block> chain_;
    uint32_t difficulty_;
};

} // namespace pp
