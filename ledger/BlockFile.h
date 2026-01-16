#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include <cstdint>
#include <fstream>
#include <string>

namespace pp {

/**
 * BlockFile manages writing block data to a single file with a size limit.
 * When the file reaches the configured size limit, it should be closed and
 * a new file should be created by BlockDir.
 */
class BlockFile : public Module {
public:
  /**
   * Configuration for BlockFile initialization
   */
  struct Config {
    std::string filepath;
    size_t maxSize{ 0 }; // Max file size (bytes)
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  /**
   * Constructor
   */
  BlockFile();

  /**
   * Destructor - ensures file is properly closed
   */
  ~BlockFile();

  /**
   * Initialize the block file
   * @param config Configuration for the block file
   * @return Roe<void> on success or error
   */
  Roe<void> init(const Config &config);

  // Delete copy constructor and assignment
  BlockFile(const BlockFile &) = delete;
  BlockFile &operator=(const BlockFile &) = delete;

  /**
   * Write block data to the file
   * @param data Block data to write
   * @param size Size of the data in bytes
   * @return Roe<int64_t> with file offset (from file start, including header)
   * where data was written, or error
   */
  Roe<int64_t> write(const void *data, uint64_t size);

  /**
   * Read block data from the file
   * @param offset File offset from file start (including header) to read from
   * @param data Buffer to read data into
   * @param size Number of bytes to read
   * @return Roe<int64_t> with number of bytes read, or error
   */
  Roe<int64_t> read(int64_t offset, void *data, size_t size);

  /**
   * Check if the file can accommodate more data
   * @param size Size of data to be written
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
   * Close the file
   */
  void close();

private:
  /**
   * File header structure for BlockFile
   * Contains magic number and version information
   */
  struct FileHeader {
    static constexpr uint32_t MAGIC =
        0x504C4642; // "PLFB" (PP Ledger File Block)
    static constexpr uint16_t CURRENT_VERSION = 1;

    uint32_t magic{ MAGIC };      // Magic number to identify BlockFile type
    uint16_t version{ CURRENT_VERSION };    // File format version
    uint16_t reserved{ 0 };   // Reserved for future use
    uint64_t headerSize{ sizeof(FileHeader) }; // Size of this header (for future extensibility)
  };

  static constexpr size_t HEADER_SIZE = sizeof(FileHeader);

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
   * Check if file has a valid header
   * @return true if header is valid, false otherwise
   */
  bool hasValidHeader() const;

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

};

} // namespace pp
