#pragma once

#include "Module.h"
#include "BlockFile.h"
#include "BlockChain.h"
#include "Block.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <functional>

namespace pp {

// Using declaration for interface type
using IBlock = iii::Block;

/**
 * Structure to represent a block's location in the storage
 */
struct BlockLocation {
    uint32_t fileId;      // ID of the file containing the block
    int64_t offset;       // Offset within the file
    size_t size;          // Size of the block data
    
    BlockLocation() : fileId(0), offset(0), size(0) {}
    BlockLocation(uint32_t fid, int64_t off, size_t sz) 
        : fileId(fid), offset(off), size(sz) {}
};

/**
 * BlockDir manages multiple block files in a directory.
 * It handles:
 * - Creating new block files when current file reaches size limit
 * - Maintaining an index file mapping block IDs to file locations
 * - Reading and writing block data across multiple files
 */
class BlockDir : public Module {
public:
    /**
     * Configuration for BlockDir initialization
     */
    struct Config {
        std::string dirPath;
        size_t maxFileSize = 100 * 1024 * 1024; // 100MB default
        
        Config() = default;
        Config(const std::string& path, size_t size = 100 * 1024 * 1024)
            : dirPath(path), maxFileSize(size) {}
    };
    
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T>
    using Roe = ResultOrError<T, Error>;
    
    /**
     * Constructor
     */
    BlockDir();
    
    /**
     * Destructor
     */
    ~BlockDir();
    
    // Delete copy constructor and assignment
    BlockDir(const BlockDir&) = delete;
    BlockDir& operator=(const BlockDir&) = delete;
    
    /**
     * Initialize the block directory
     * @param config Configuration for the block directory
     * @param manageBlockchain If true, this BlockDir will manage a BlockChain instance
     * @return Roe<void> on success or error
     */
    Roe<void> init(const Config& config, bool manageBlockchain = false);
    
    /**
     * Move front file to another BlockDir and append its blocks to that directory's index
     * @param targetDir Target BlockDir to move the front file to
     * @return Roe<void> on success or error
     */
    Roe<void> moveFrontFileTo(BlockDir& targetDir);
    
    /**
     * Flush all data and index to disk
     */
    void flush();
    
    /**
     * Get total storage size used by all block files (in bytes)
     */
    size_t getTotalStorageSize() const;
    
    // Blockchain management (only available if manageBlockchain is true)
    /**
     * Add a block to the blockchain
     * @param block Block to add
     * @return true on success, false on error
     */
    bool addBlock(std::shared_ptr<IBlock> block);
    
    /**
     * Get the latest block from the blockchain
     * @return Latest block or nullptr if chain is empty
     */
    std::shared_ptr<IBlock> getLatestBlock() const;
    
    /**
     * Get the size of the blockchain
     * @return Number of blocks in the chain
     */
    size_t getBlockchainSize() const;
    
    /**
     * Get a block by index
     * @param index Block index
     * @return Block or nullptr if not found
     */
    std::shared_ptr<IBlock> getBlock(uint64_t index) const;
    
    /**
     * Check if the blockchain is valid
     * @return true if valid, false otherwise
     */
    bool isBlockchainValid() const;
    
    /**
     * Get the last block hash
     * @return Hash of the last block, or "0" if empty
     */
    std::string getLastBlockHash() const;
    
    /**
     * Trim blocks from the blockchain
     * @param blockIndices Block indices to remove
     * @return Number of blocks removed
     */
    size_t trimBlocks(const std::vector<uint64_t>& blockIndices);

private:
    std::string dirPath_;
    size_t maxFileSize_;
    uint32_t currentFileId_;
    
    // Block files indexed by file ID
    std::unordered_map<uint32_t, std::unique_ptr<BlockFile>> ukpBlockFiles_;
    
    // Ordered list of file IDs (tracks creation/addition order)
    std::vector<uint32_t> fileIdOrder_;
    
    // Index mapping block ID to location
    std::unordered_map<uint64_t, BlockLocation> blockIndex_;
    
    // Path to the index file
    std::string indexFilePath_;
    
    // Blockchain instance (only used if manageBlockchain is true)
    std::unique_ptr<BlockChain> ukpBlockchain_;
    bool managesBlockchain_;
    
    /**
     * Write a block to storage
     * @param blockId Unique identifier for the block
     * @param data Block data to write
     * @param size Size of the data in bytes
     * @return Roe<void> on success or error
     */
    Roe<void> writeBlock(uint64_t blockId, const void* data, size_t size);
    
    /**
     * Check if a block exists in storage
     * @param blockId Unique identifier for the block
     * @return true if block exists, false otherwise
     */
    bool hasBlock(uint64_t blockId) const;
    
    /**
     * Get the ID of the front (oldest) file
     * @return File ID of the oldest file, or 0 if no files exist
     */
    uint32_t getFrontFileId() const;
    
    /**
     * Remove and return the front (oldest) block file
     * @return Unique pointer to the BlockFile, or nullptr if no files exist
     */
    std::unique_ptr<BlockFile> popFrontFile();
    
    /**
     * Create a new block file
     * @param fileId ID for the new file
     * @return Pointer to the new BlockFile, or nullptr on error
     */
    BlockFile* createBlockFile(uint32_t fileId);
    
    /**
     * Get or create the current active block file for writing
     * @param dataSize Size of data to be written (to check if file can fit)
     * @return Pointer to the BlockFile, or nullptr on error
     */
    BlockFile* getActiveBlockFile(size_t dataSize);
    
    /**
     * Get block file by ID
     * @param fileId File ID
     * @return Pointer to the BlockFile, or nullptr if not found
     */
    BlockFile* getBlockFile(uint32_t fileId);
    
    /**
     * Generate file path for a block file
     * @param fileId File ID
     * @return File path
     */
    std::string getBlockFilePath(uint32_t fileId) const;
    
    /**
     * Load the index file from disk
     * @return true on success, false on error
     */
    bool loadIndex();
    
    /**
     * Save the index file to disk
     * @return true on success, false on error
     */
    bool saveIndex();
};

} // namespace pp
