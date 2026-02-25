#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace pp {

/**
 * FileStore manages writing block data to a single file with a size limit.
 * 
 * File format:
 * - Header: magic, version, blockCount, headerSize
 * - Block data: [size (8 bytes)][data (size bytes)]*
 * 
 * Block sizes are stored at the beginning of each block. On file close,
 * the total block count is written to the header. On first read by index,
 * the block index (offsets) is built by scanning the file.
 */
class FileStore : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  /**
   * Configuration for FileStore initialization
   */
  struct InitConfig {
    std::string filepath;
    size_t maxSize{ 0 }; // Max file size (bytes)
  };

  /**
   * Constructor
   */
  FileStore();

  /**
   * Destructor - ensures file is properly closed
   */
  ~FileStore() override;

  /**
   * Initialize the file store (creates new file)
   * @param config Configuration for the file store
   * @return Roe<void> on success or error
   */
  Roe<void> init(const InitConfig &config);

  /**
   * Mount an existing file store
   * @param filepath Path to existing file
   * @param maxSize Maximum file size
   * @return Roe<void> on success or error
   */
  Roe<void> mount(const std::string &filepath, size_t maxSize);

  // Delete copy constructor and assignment
  FileStore(const FileStore &) = delete;
  FileStore &operator=(const FileStore &) = delete;

  // Block store interface
  Roe<std::string> readBlock(uint64_t index) const;
  Roe<uint64_t> appendBlock(const std::string &block);
  Roe<void> rewindTo(uint64_t index);

  /**
   * Write block data to the file (with size prefix)
   * @param data Block data to write
   * @param size Size of the data in bytes
   * @return Roe<int64_t> with block index (0-based within this file), or error
   */
  Roe<int64_t> write(const void *data, uint64_t size);

  /**
   * Read block data by index (0-based, within this file)
   * Lazily builds the block index on first call if not already built.
   * @param index Block index within this file (0-based)
   * @param data Buffer to read data into (must be large enough)
   * @param maxSize Maximum size of the buffer
   * @return Roe<int64_t> with number of bytes read (block size), or error
   */
  Roe<int64_t> readBlock(uint64_t index, void *data, size_t maxSize);

  /**
   * Get the size of a block by index (0-based, within this file)
   * Lazily builds the block index on first call if not already built.
   * @param index Block index within this file (0-based)
   * @return Roe<uint64_t> with block size, or error
   */
  Roe<uint64_t> getBlockSize(uint64_t index);

  /**
   * Get the number of blocks stored in this file
   * @return Number of blocks
   */
  uint64_t getBlockCount() const { return blockCount_; }

  /**
   * Check if the file can accommodate more data
   * @param size Size of data to be written (excluding size prefix)
   * @return true if data can fit, false otherwise
   */
  bool canFit(uint64_t size) const;

  /**
   * Get current file size (including header)
   */
  size_t getCurrentSize() const { return currentSize_; }

  /**
   * Get maximum file size
   */
  size_t getMaxSize() const { return maxSize_; }

  /**
   * Get file path
   */
  const std::string &getFilePath() const { return filepath_; }

  /**
   * Check if file is open and ready for operations
   */
  bool isOpen() const;

  /**
   * Close the file (writes block count to header)
   */
  void close();

private:
  /**
   * File header structure for FileStore
   * Contains magic number, version, and block count
   */
  struct FileHeader {
    static constexpr uint32_t MAGIC =
        0x504C4642; // "PLFB" (PP Ledger File Block)
    static constexpr uint16_t CURRENT_VERSION = 1;

    uint32_t magic{ MAGIC };      // Magic number to identify FileStore type
    uint16_t version{ CURRENT_VERSION };    // File format version
    uint16_t reserved{ 0 };       // Reserved for future use
    uint64_t blockCount{ 0 };     // Number of blocks stored in this file
    uint64_t headerSize{ sizeof(FileHeader) }; // Size of this header (for future extensibility)
  };

  /**
   * Block entry in the index (offset and size)
   */
  struct BlockEntry {
    int64_t offset;   // Offset to the size prefix in the file
    uint64_t size;    // Size of the block data (excluding size prefix)
    
    BlockEntry() : offset(0), size(0) {}
    BlockEntry(int64_t off, uint64_t sz) : offset(off), size(sz) {}
  };

  static constexpr size_t HEADER_SIZE = sizeof(FileHeader);
  static constexpr size_t SIZE_PREFIX_BYTES = sizeof(uint64_t);

  /**
   * Open the file for reading and writing
   * @return Roe<void> on success or error
   */
  Roe<void> open();

  /**
   * Write the file header to a new file
   * @return Roe<void> on success or error
   */
  Roe<void> writeHeader();

  /**
   * Read and validate the file header
   * @return Roe<void> on success or error
   */
  Roe<void> readHeader();

  /**
   * Update the block count in the file header
   * @return Roe<void> on success or error
   */
  Roe<void> updateHeaderBlockCount();

  /**
   * Check if file has a valid header
   * @return true if header is valid, false otherwise
   */
  bool hasValidHeader() const;

  /**
   * Build the block index by scanning the file
   * Called lazily on first read by index
   * @return Roe<void> on success or error
   */
  Roe<void> buildBlockIndex();

  /**
   * Ensure block index is built
   * @return Roe<void> on success or error
   */
  Roe<void> ensureBlockIndex();

  /**
   * Flush any buffered data to disk
   */
  void flush();

  /**
   * Get the header offset (always 0, but useful for clarity)
   */
  static constexpr int64_t getHeaderOffset() { return 0; }

  /**
   * Get the data offset (where actual block data starts)
   */
  static constexpr int64_t getDataOffset() { return HEADER_SIZE; }

  // ------ Private members ------
  std::string filepath_;
  size_t maxSize_{ 0 };
  size_t currentSize_{ 0 };
  std::fstream file_;
  FileHeader header_;
  bool headerValid_{ false };
  
  // Block tracking
  uint64_t blockCount_{ 0 };           // Number of blocks written/loaded
  std::vector<BlockEntry> blockIndex_; // Index of block offsets and sizes
  bool indexBuilt_{ false };           // Whether block index has been built

};

} // namespace pp
