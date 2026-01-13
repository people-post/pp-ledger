#pragma once

#include "Module.h"
#include "BlockFile.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace pp {

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
     * @return Roe<void> on success or error
     */
    Roe<void> init(const Config& config);
    
    /**
     * Write a block to storage
     * @param blockId Unique identifier for the block
     * @param data Block data to write
     * @param size Size of the data in bytes
     * @return Roe<void> on success or error
     */
    Roe<void> writeBlock(uint64_t blockId, const void* data, size_t size);
    
    /**
     * Read a block from storage
     * @param blockId Unique identifier for the block
     * @param data Buffer to read data into (must be pre-allocated)
     * @param maxSize Maximum size of the buffer
     * @return Roe<int64_t> with number of bytes read, or error
     */
    Roe<int64_t> readBlock(uint64_t blockId, void* data, size_t maxSize);
    
    /**
     * Get the location information for a block
     * @param blockId Unique identifier for the block
     * @param location Output parameter for block location
     * @return true if block exists, false otherwise
     */
    bool getBlockLocation(uint64_t blockId, BlockLocation& location) const;
    
    /**
     * Check if a block exists in storage
     * @param blockId Unique identifier for the block
     * @return true if block exists, false otherwise
     */
    bool hasBlock(uint64_t blockId) const;
    
    /**
     * Flush all data and index to disk
     */
    void flush();
    
    /**
     * Get the directory path
     */
    const std::string& getDirPath() const { return dirPath_; }
    
    /**
     * Get number of block files
     */
    size_t getFileCount() const { return ukpBlockFiles_.size(); }
    
    /**
     * Get total number of blocks stored
     */
    size_t getBlockCount() const { return blockIndex_.size(); }

private:
    std::string dirPath_;
    size_t maxFileSize_;
    uint32_t currentFileId_;
    
    // Block files indexed by file ID
    std::unordered_map<uint32_t, std::unique_ptr<BlockFile>> ukpBlockFiles_;
    
    // Index mapping block ID to location
    std::unordered_map<uint64_t, BlockLocation> blockIndex_;
    
    // Path to the index file
    std::string indexFilePath_;
    
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
