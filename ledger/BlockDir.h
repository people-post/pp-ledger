#pragma once

#include "Block.h"
#include "BlockChain.h"
#include "FileStore.h"
#include "Module.h"
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

// Using declaration for interface type
using IBlock = iii::Block;

/**
 * BlockDir manages multiple block files in a directory.
 * It handles:
 * - Creating new block files when current file reaches size limit
 * - Maintaining an index file mapping file IDs to starting block indices
 * - Reading and writing block data across multiple files
 * 
 * Block indices are assumed to be sequential within a file and across files.
 * BlockDir only stores each file's starting block index; FileStore handles
 * individual block indexing within each file.
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
    Config(const std::string &path, size_t size = 100 * 1024 * 1024)
        : dirPath(path), maxFileSize(size) {}
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  /**
   * Constructor
   */
  BlockDir();

  /**
   * Destructor
   */
  ~BlockDir();

  // Delete copy constructor and assignment
  BlockDir(const BlockDir &) = delete;
  BlockDir &operator=(const BlockDir &) = delete;

  /**
   * Initialize the block directory
   * @param config Configuration for the block directory
   * @param manageBlockchain If true, this BlockDir will manage a BlockChain
   * instance
   * @return Roe<void> on success or error
   */
  Roe<void> init(const Config &config, bool manageBlockchain = false);

  /**
   * Move front file to another BlockDir and append its blocks to that
   * directory's index
   * @param targetDir Target BlockDir to move the front file to
   * @return Roe<void> on success or error
   */
  Roe<void> moveFrontFileTo(BlockDir &targetDir);

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
   * Index file header structure
   * Contains magic number and version information
   * Simplified format - FileStore now handles individual block indexing
   */
  struct IndexFileHeader {
    static constexpr uint32_t MAGIC =
        0x504C4944; // "PLID" (PP Ledger Index Directory)
    static constexpr uint16_t CURRENT_VERSION = 1;

    uint32_t magic;      // Magic number to identify index file type
    uint16_t version;    // File format version
    uint16_t reserved;   // Reserved for future use
    uint64_t headerSize; // Size of this header (for future extensibility)

    IndexFileHeader()
        : magic(MAGIC), version(CURRENT_VERSION), reserved(0),
          headerSize(sizeof(IndexFileHeader)) {}

    /**
     * Serialize using Archive pattern
     */
    template <typename Archive> void serialize(Archive &ar) {
      ar &magic &version &reserved &headerSize;
    }
  };

  /**
   * Structure representing a file's starting block index
   * Block indices are sequential starting from startBlockId
   * FileStore stores the actual block count and handles per-block indexing
   */
  struct FileIndexEntry {
    uint32_t fileId;
    uint64_t startBlockId;

    FileIndexEntry() : fileId(0), startBlockId(0) {}
    FileIndexEntry(uint32_t fid, uint64_t startId) 
        : fileId(fid), startBlockId(startId) {}

    /**
     * Serialize using Archive pattern
     */
    template <typename Archive> void serialize(Archive &ar) {
      ar &fileId &startBlockId;
    }
  };

  static constexpr size_t INDEX_HEADER_SIZE = sizeof(IndexFileHeader);

  /**
   * Structure holding FileStore and its starting block index
   */
  struct FileInfo {
    std::unique_ptr<FileStore> blockFile;
    uint64_t startBlockId; // Starting block index for this file
  };

  std::string dirPath_;
  size_t maxFileSize_{ 0 };
  uint32_t currentFileId_{ 0 };

  // Block files with their starting block indices, indexed by file ID
  std::unordered_map<uint32_t, FileInfo> fileInfoMap_;

  // Ordered list of file IDs (tracks creation/addition order)
  std::vector<uint32_t> fileIdOrder_;

  // Total block count across all files
  uint64_t totalBlockCount_{ 0 };

  // Path to the index file
  std::string indexFilePath_;

  // Blockchain instance (only used if manageBlockchain is true)
  std::unique_ptr<BlockChain> ukpBlockchain_;
  bool managesBlockchain_{ false };

  /**
   * Write a block to storage
   * @param blockId Unique identifier for the block
   * @param data Block data to write
   * @param size Size of the data in bytes
   * @return Roe<void> on success or error
   */
  Roe<void> writeBlock(uint64_t blockId, const void *data, uint64_t size);

  /**
   * Read a block from storage by block ID
   * @param blockId Unique identifier for the block
   * @param data Buffer to read data into
   * @param maxSize Maximum size of the buffer
   * @return Roe<int64_t> with number of bytes read, or error
   */
  Roe<int64_t> readBlock(uint64_t blockId, void *data, size_t maxSize) const;

  /**
   * Check if a block exists in storage
   * @param blockId Unique identifier for the block
   * @return true if block exists, false otherwise
   */
  bool hasBlock(uint64_t blockId) const;

  /**
   * Find the file containing a specific block ID
   * @param blockId Block ID to find
   * @return Pair of (fileId, indexWithinFile), or (0, 0) if not found
   */
  std::pair<uint32_t, uint64_t> findBlockFile(uint64_t blockId) const;

  /**
   * Get the ID of the front (oldest) file
   * @return File ID of the oldest file, or 0 if no files exist
   */
  uint32_t getFrontFileId() const;

  /**
   * Remove and return the front (oldest) block file
   * @return Unique pointer to the FileStore, or nullptr if no files exist
   */
  std::unique_ptr<FileStore> popFrontFile();

  /**
   * Create a new block file
   * @param fileId ID for the new file
   * @param startBlockId Starting block index for this file
   * @return Pointer to the new FileStore, or nullptr on error
   */
  FileStore *createBlockFile(uint32_t fileId, uint64_t startBlockId);

  /**
   * Get or create the current active block file for writing
   * @param dataSize Size of data to be written (to check if file can fit)
   * @return Pointer to the FileStore, or nullptr on error
   */
  FileStore *getActiveBlockFile(uint64_t dataSize);

  /**
   * Get block file by ID (opens it if not already open)
   * @param fileId File ID
   * @return Pointer to the FileStore, or nullptr if not found
   */
  FileStore *getBlockFile(uint32_t fileId);

  /**
   * Get block file by ID (const version, doesn't open files)
   * @param fileId File ID
   * @return Pointer to the FileStore, or nullptr if not found/not open
   */
  const FileStore *getBlockFileConst(uint32_t fileId) const;

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
  bool writeIndexHeader(std::ostream &os);

  /**
   * Read and validate the index file header
   * @param is Input stream
   * @return true on success, false on error
   */
  bool readIndexHeader(std::istream &is);

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
