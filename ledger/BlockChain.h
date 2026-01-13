#pragma once

#include "Block.h"
#include "Module.h"
#include "../interface/BlockChain.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace pp {

// Using declaration for interface type
using IBlockChain = iii::BlockChain;

/**
 * Concrete implementation of BlockChain interface
 */
class BlockChain : public Module, public IBlockChain {
public:
    BlockChain();
    ~BlockChain() = default;
    
    // IBlockChain interface implementation
    bool addBlock(std::shared_ptr<IBlock> block) override;
    std::shared_ptr<IBlock> getLatestBlock() const override;
    std::shared_ptr<IBlock> getBlock(uint64_t index) const override;
    size_t getSize() const override;
    bool isValid() const override;
    bool validateBlock(const IBlock& block) const override;
    std::vector<std::shared_ptr<IBlock>> getBlocks(uint64_t fromIndex, uint64_t toIndex) const override;
    std::string getLastBlockHash() const override;
    
    // Configuration
    void setDifficulty(uint32_t difficulty);
    uint32_t getDifficulty() const;
    
private:
    void createGenesisBlock();
    
    std::vector<std::shared_ptr<IBlock>> chain_;
    uint32_t difficulty_;
};

} // namespace pp
