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
#include <ostream>
#include <istream>

namespace pp {

// Using declaration for interface type
using IBlock = iii::Block;

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
     * Get total storage size used by all block files (in bytes)
     */
    size_t getTotalStorageSize() const;
    
    // Blockchain management (only available if manageBlockchain is true)
    /**
     * Add a block to the blockchain
     * The block index will be automatically set to the next sequential index
     * (current blockchain size).
     * @param block Block to add (index will be set automatically)
     * @return true on success, false on error
     */
    bool addBlock(std::shared_ptr<Block> block);
    
    /**
     * Get the latest block from the blockchain
     * @return Latest block or nullptr if chain is empty
     */
    std::shared_ptr<Block> getLatestBlock() const;
    
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
    std::shared_ptr<Block> getBlock(uint64_t index) const;
    
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

private:
    /**
     * Structure representing block offset and size
     * Used in IndexEntry to store sequential block locations
     */
    struct BlockOffsetSize {
        int64_t offset;   // Offset within the file (from file start, including header)
        uint64_t size;   // Size of the block data
        
        BlockOffsetSize() : offset(0), size(0) {}
        BlockOffsetSize(int64_t off, uint64_t sz) : offset(off), size(sz) {}
        
        /**
         * Serialize using Archive pattern
         */
        template<typename Archive>
        void serialize(Archive& ar) {
            ar & offset & size;
        }
    };
    
    /**
     * Structure to represent a block's location in the storage
     */
    struct BlockLocation {
        uint32_t fileId;      // ID of the file containing the block
        BlockOffsetSize offsetSize;  // Offset and size of the block data
        
        BlockLocation() : fileId(0) {}
        BlockLocation(uint32_t fid, int64_t off, uint64_t sz) 
            : fileId(fid), offsetSize(off, sz) {}
        BlockLocation(uint32_t fid, const BlockOffsetSize& os)
            : fileId(fid), offsetSize(os) {}
        
        // Convenience accessors
        int64_t offset() const { return offsetSize.offset; }
        uint64_t size() const { return offsetSize.size; }
        
        /**
         * Serialize using Archive pattern
         */
        template<typename Archive>
        void serialize(Archive& ar) {
            ar & fileId & offsetSize;
        }
    };
    
    /**
     * Structure representing a file's block range with locations
     * Groups startBlockId and blockLocations together since they are logically related
     * Block IDs are sequential starting from startBlockId, so no need to store blockId for each block
     */
    struct FileBlockRange {
        uint64_t startBlockId;
        std::vector<BlockOffsetSize> blockLocations;  // Sequential (offset, size) pairs
        
        FileBlockRange() : startBlockId(0) {}
        FileBlockRange(uint64_t startId) : startBlockId(startId) {}
        
        /**
         * Serialize using Archive pattern
         */
        template<typename Archive>
        void serialize(Archive& ar) {
            ar & startBlockId & blockLocations;
        }
    };
    
    /**
     * Structure representing an index file entry
     * Stores file ID and the file's block range with locations
     * Binary format: [fileId (4 bytes)][startBlockId (8 bytes)][blockCount (8 bytes)][(offset, size)*]
     */
    struct IndexEntry {
        uint32_t fileId;
        FileBlockRange blockRange;
        
        IndexEntry() : fileId(0) {}
        IndexEntry(uint32_t fid, uint64_t startId)
            : fileId(fid), blockRange(startId) {}
        
        /**
         * Serialize using Archive pattern
         */
        template<typename Archive>
        void serialize(Archive& ar) {
            ar & fileId & blockRange;
        }
    };
    
    /**
     * Index file header structure
     * Contains magic number and version information
     */
    struct IndexFileHeader {
        static constexpr uint32_t MAGIC = 0x504C4944; // "PLID" (PP Ledger Index Directory)
        static constexpr uint16_t CURRENT_VERSION = 1;
        
        uint32_t magic;        // Magic number to identify index file type
        uint16_t version;      // File format version
        uint16_t reserved;     // Reserved for future use
        uint64_t headerSize;   // Size of this header (for future extensibility)
        
        IndexFileHeader() 
            : magic(MAGIC)
            , version(CURRENT_VERSION)
            , reserved(0)
            , headerSize(sizeof(IndexFileHeader)) {}
        
        /**
         * Serialize using Archive pattern
         */
        template<typename Archive>
        void serialize(Archive& ar) {
            ar & magic & version & reserved & headerSize;
        }
    };
    
    static constexpr size_t INDEX_HEADER_SIZE = sizeof(IndexFileHeader);
    
    /**
     * Structure holding BlockFile and its block range
     */
    struct FileInfo {
        std::unique_ptr<BlockFile> blockFile;
        FileBlockRange blockRange;  // Block range with locations
    };
    
    std::string dirPath_;
    size_t maxFileSize_;
    uint32_t currentFileId_;
    
    // Block files with their block ID ranges indexed by file ID
    std::unordered_map<uint32_t, FileInfo> fileInfoMap_;
    
    // Ordered list of file IDs (tracks creation/addition order)
    std::vector<uint32_t> fileIdOrder_;
    
    // Index mapping block ID to location (built during loading from ranges)
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
    Roe<void> writeBlock(uint64_t blockId, const void* data, uint64_t size);
    
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
    BlockFile* getActiveBlockFile(uint64_t dataSize);
    
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
    
    /**
     * Write the index file header
     * @param os Output stream
     * @return true on success, false on error
     */
    bool writeIndexHeader(std::ostream& os);
    
    /**
     * Read and validate the index file header
     * @param is Input stream
     * @return true on success, false on error
     */
    bool readIndexHeader(std::istream& is);
    
    /**
     * Flush all data and index to disk
     */
    void flush();
    
    /**
     * Trim blocks from the head of the blockchain
     * @param count Number of blocks to trim from the head
     * @return Number of blocks removed
     */
    size_t trimBlocks(size_t count);
    
    /**
     * Populate blockchain from existing blocks in storage
     * Reads all blocks from storage and adds them to blockchain in order
     * @return Roe<void> on success or error
     */
    Roe<void> populateBlockchainFromStorage();
    
    /**
     * Load blocks from a specific file into the blockchain
     * @param fileId File ID to load blocks from
     * @return Roe<size_t> with number of blocks successfully loaded, or error
     */
    Roe<size_t> loadBlocksFromFile(uint32_t fileId);
};

} // namespace pp
