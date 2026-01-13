#pragma once

#include "Block.h"
#include "BlockDir.h"
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
 * 
 * Manages blocks across two BlockDirs:
 * - Active BlockDir: Recently added blocks (hot storage)
 * - Archive BlockDir: Older blocks (cold storage)
 * 
 * Files are transferred from active to archive based on:
 * - Storage usage threshold (maxActiveDirSize)
 * - One complete file is moved as the basic unit
 */
class BlockChain : public Module, public IBlockChain {
public:
    /**
     * Configuration for BlockChain storage
     */
    struct StorageConfig {
        std::string activeDirPath;      // Path for active (hot) blocks
        std::string archiveDirPath;     // Path for archived (cold) blocks
        size_t maxActiveDirSize;        // Max size of active directory before transferring files (bytes)
        size_t blockDirFileSize;        // Max file size for BlockDir files
        
        StorageConfig() 
            : activeDirPath("./blockchain_active"),
              archiveDirPath("./blockchain_archive"),
              maxActiveDirSize(500 * 1024 * 1024), // 500MB default
              blockDirFileSize(100 * 1024 * 1024) {}
    };

    BlockChain();
    ~BlockChain() = default;
    
    // IBlockChain interface implementation (consensus methods)
    std::shared_ptr<IBlock> getLatestBlock() const override;
    size_t getSize() const override;
    
    // Additional blockchain operations
    bool addBlock(std::shared_ptr<IBlock> block);
    std::shared_ptr<IBlock> getBlock(uint64_t index) const;
    bool isValid() const;
    std::string getLastBlockHash() const;
    
    // Configuration
    void setDifficulty(uint32_t difficulty);
    uint32_t getDifficulty() const;
    
    /**
     * Initialize storage directories
     * @param config Storage configuration
     * @return true on success, false on error
     */
    bool initStorage(const StorageConfig& config);
    
    /**
     * Get the max active directory size (bytes before transferring to archive)
     */
    size_t getMaxActiveDirSize() const { return maxActiveDirSize_; }
    
    /**
     * Set the max active directory size (bytes before transferring to archive)
     */
    void setMaxActiveDirSize(size_t bytes) { maxActiveDirSize_ = bytes; }
    
private:
    void createGenesisBlock();
    
    /**
     * Transfer blocks from active to archive directory
     * Called when active directory reaches the transfer threshold
     */
    void transferBlocksToArchive();
    
    // Internal helper methods (not part of interface)
    bool validateBlock(const IBlock& block) const;
    std::vector<std::shared_ptr<IBlock>> getBlocks(uint64_t fromIndex, uint64_t toIndex) const;
    
    std::vector<std::shared_ptr<IBlock>> chain_;
    uint32_t difficulty_;
    
    // Storage management
    std::unique_ptr<BlockDir> activeBlockDir_;   // Hot storage for recent blocks
    std::unique_ptr<BlockDir> archiveBlockDir_;  // Cold storage for older blocks
    size_t maxActiveDirSize_;                     // Max size of active directory (bytes)
};

} // namespace pp
