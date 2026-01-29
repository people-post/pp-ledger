#include "FileStore.h"
#include "Logger.h"
#include <filesystem>

namespace pp {

FileStore::FileStore() {}

FileStore::~FileStore() { close(); }

FileStore::Roe<void> FileStore::init(const InitConfig &config) {
  filepath_ = config.filepath;
  maxSize_ = config.maxSize;
  currentSize_ = 0;
  headerValid_ = false;
  blockCount_ = 0;
  blockIndex_.clear();
  indexBuilt_ = false;

  if (maxSize_ < 1024 * 1024) {
    return Error("Max file size shall be at least 1MB");
  }

  if (filepath_.empty()) {
    return Error("Filepath is not set");
  }

  // Verify file does NOT exist (fresh initialization)
  if (std::filesystem::exists(filepath_)) {
    return Error("File already exists: " + filepath_ + ". Use mount() to load existing file.");
  }

  auto result = open();
  if (!result) {
    log().error << "Failed to create file: " << filepath_;
    return result.error();
  }

  // Write header for new file
  auto headerResult = writeHeader();
  if (!headerResult) {
    log().error << "Failed to write header to new file: " << filepath_;
    return headerResult.error();
  }
  currentSize_ = HEADER_SIZE;
  log().debug << "Created new file with header: " << filepath_;

  return {};
}

FileStore::Roe<void> FileStore::mount(const std::string &filepath, size_t maxSize) {
  filepath_ = filepath;
  maxSize_ = maxSize;
  currentSize_ = 0;
  headerValid_ = false;
  blockCount_ = 0;
  blockIndex_.clear();
  indexBuilt_ = false;

  if (maxSize_ < 1024 * 1024) {
    return Error("Max file size shall be at least 1MB");
  }

  if (filepath_.empty()) {
    return Error("Filepath is not set");
  }

  // Verify file exists (mounting existing file)
  if (!std::filesystem::exists(filepath_)) {
    return Error("File does not exist: " + filepath_ + ". Use init() to create new file.");
  }

  auto result = open();
  if (!result) {
    log().error << "Failed to open file: " << filepath_;
    return result.error();
  }

  // Read and validate existing header
  auto headerResult = readHeader();
  if (!headerResult) {
    log().error << "Failed to read header from existing file: " << filepath_;
    return headerResult.error();
  }

  // Get total file size (including header)
  currentSize_ = std::filesystem::file_size(filepath_);
  
  // Load block count from header
  blockCount_ = header_.blockCount;
  log().debug << "Mounted existing file: " << filepath_
              << " (total size: " << currentSize_
              << " bytes, version: " << header_.version
              << ", blocks: " << blockCount_ << ")";

  return {};
}

FileStore::Roe<void> FileStore::open() {
  // Open file in binary mode for both reading and writing
  // For existing files, we'll use in|out mode (not app) so we can read header
  // For new files, we'll create them first
  bool fileExists = std::filesystem::exists(filepath_);

  if (fileExists) {
    // Open existing file for reading and writing
    file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out);
  } else {
    // Create new file
    file_.open(filepath_, std::ios::binary | std::ios::out);
    if (file_.is_open()) {
      file_.close();
      // Reopen for reading and writing
      file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out);
    }
  }

  if (!file_.is_open()) {
    return Error("Failed to open file: " + filepath_);
  }

  return {};
}

FileStore::Roe<int64_t> FileStore::write(const void *data, uint64_t size) {
  if (!isOpen()) {
    log().error << "File is not open: " << filepath_;
    return Error("File is not open: " + filepath_);
  }

  if (!hasValidHeader()) {
    log().error << "File header is not valid: " << filepath_;
    return Error("File header is not valid: " + filepath_);
  }

  // canFit now accounts for size prefix
  if (!canFit(size)) {
    log().warning << "Cannot fit " << size << " bytes + " << SIZE_PREFIX_BYTES
                  << " prefix (current: " << currentSize_
                  << ", max: " << maxSize_ << ")";
    return Error("Cannot fit " + std::to_string(size) + " bytes");
  }

  // Seek to end of file
  file_.seekp(0, std::ios::end);
  int64_t fileOffset = file_.tellp();

  // Write size prefix (8 bytes)
  file_.write(reinterpret_cast<const char *>(&size), SIZE_PREFIX_BYTES);

  if (!file_.good()) {
    log().error << "Failed to write size prefix to file: " << filepath_;
    return Error("Failed to write size prefix to file: " + filepath_);
  }

  // Write block data
  file_.write(static_cast<const char *>(data), size);

  if (!file_.good()) {
    log().error << "Failed to write data to file: " << filepath_;
    return Error("Failed to write data to file: " + filepath_);
  }

  // Flush data to disk
  file_.flush();
  if (!file_.good()) {
    log().error << "Failed to flush data to file: " << filepath_;
    return Error("Failed to flush data to file: " + filepath_);
  }

  // Update block index (always keep in sync)
  blockIndex_.push_back(BlockEntry(fileOffset, size));
  indexBuilt_ = true;

  // Get block index before incrementing count
  int64_t blockIdx = static_cast<int64_t>(blockCount_);
  
  // Update block count and file size
  blockCount_++;
  currentSize_ += SIZE_PREFIX_BYTES + size;
  
  // Update block count in header immediately (for durability)
  auto headerResult = updateHeaderBlockCount();
  if (!headerResult.isOk()) {
    log().warning << "Failed to update header block count: " 
                  << headerResult.error().message;
  }
  
  log().debug << "Wrote block " << blockIdx << " (" << size 
              << " bytes) at file offset " << fileOffset
              << " (total file size: " << currentSize_ << ")";

  // Return block index
  return blockIdx;
}

FileStore::Roe<int64_t> FileStore::readBlock(uint64_t index, void *data,
                                             size_t maxSize) {
  if (!isOpen()) {
    log().error << "File is not open: " << filepath_;
    return Error("File is not open: " + filepath_);
  }

  if (!hasValidHeader()) {
    log().error << "File header is not valid: " << filepath_;
    return Error("File header is not valid: " + filepath_);
  }

  // Ensure block index is built
  auto indexResult = ensureBlockIndex();
  if (!indexResult.isOk()) {
    return indexResult.error();
  }

  if (index >= blockIndex_.size()) {
    return Error("Block index " + std::to_string(index) + " out of range (max: " +
                 std::to_string(blockIndex_.size()) + ")");
  }

  const BlockEntry &entry = blockIndex_[index];
  
  if (entry.size > maxSize) {
    return Error("Buffer too small for block " + std::to_string(index) +
                 " (need: " + std::to_string(entry.size) +
                 ", have: " + std::to_string(maxSize) + ")");
  }

  // Calculate data offset (skip size prefix)
  int64_t dataOffset = entry.offset + static_cast<int64_t>(SIZE_PREFIX_BYTES);

  // Clear any stream errors and seek to data position
  file_.clear();
  file_.seekg(dataOffset, std::ios::beg);

  if (!file_.good()) {
    log().error << "Failed to seek to offset " << dataOffset
                << " in file: " << filepath_;
    return Error("Failed to seek to offset " + std::to_string(dataOffset));
  }

  // Read block data
  file_.read(static_cast<char *>(data), entry.size);

  int64_t bytesRead = file_.gcount();

  if (bytesRead != static_cast<int64_t>(entry.size)) {
    log().warning << "Read " << bytesRead << " bytes, expected " << entry.size;
    return Error("Failed to read complete block data");
  }

  return static_cast<int64_t>(entry.size);
}

FileStore::Roe<uint64_t> FileStore::getBlockSize(uint64_t index) {
  // Ensure block index is built
  auto indexResult = ensureBlockIndex();
  if (!indexResult.isOk()) {
    return indexResult.error();
  }

  if (index >= blockIndex_.size()) {
    return Error("Block index " + std::to_string(index) + " out of range (max: " +
                 std::to_string(blockIndex_.size()) + ")");
  }

  return blockIndex_[index].size;
}

bool FileStore::canFit(uint64_t size) const {
  // currentSize_ already includes header, add size prefix overhead
  return (currentSize_ + SIZE_PREFIX_BYTES + size) <= maxSize_;
}

bool FileStore::isOpen() const { return file_.is_open() && file_.good(); }

void FileStore::close() {
  if (file_.is_open()) {
    // Update block count in header before closing
    auto result = updateHeaderBlockCount();
    if (!result.isOk()) {
      log().warning << "Failed to update header block count: " 
                    << result.error().message;
    }
    
    file_.close();
    log().debug << "Closed file: " << filepath_ << " (blocks: " << blockCount_ << ")";
  }
}

void FileStore::flush() {
  if (file_.is_open()) {
    file_.flush();
  }
}

FileStore::Roe<void> FileStore::writeHeader() {
  if (!isOpen()) {
    return Error("File is not open: " + filepath_);
  }

  // Seek to beginning of file
  file_.seekp(0, std::ios::beg);

  // Initialize header
  header_ = FileHeader();
  header_.blockCount = blockCount_;

  // Write header
  file_.write(reinterpret_cast<const char *>(&header_), sizeof(FileHeader));

  if (!file_.good()) {
    return Error("Failed to write header to file: " + filepath_);
  }

  // Flush header to disk
  file_.flush();
  if (!file_.good()) {
    return Error("Failed to flush header to file: " + filepath_);
  }

  headerValid_ = true;
  log().debug << "Wrote file header (magic: 0x" << std::hex << header_.magic
              << std::dec << ", version: " << header_.version 
              << ", blocks: " << header_.blockCount << ")";

  return {};
}

FileStore::Roe<void> FileStore::readHeader() {
  if (!isOpen()) {
    return Error("File is not open: " + filepath_);
  }

  // Seek to beginning of file
  file_.seekg(0, std::ios::beg);

  // Read header
  file_.read(reinterpret_cast<char *>(&header_), sizeof(FileHeader));

  if (file_.gcount() != static_cast<std::streamsize>(sizeof(FileHeader))) {
    return Error("Failed to read complete header from file: " + filepath_);
  }

  // Validate header
  if (header_.magic != FileHeader::MAGIC) {
    return Error("Invalid magic number in file header: " + filepath_);
  }

  if (header_.version > FileHeader::CURRENT_VERSION) {
    return Error("Unsupported file version " + std::to_string(header_.version) +
                 " (current: " + std::to_string(FileHeader::CURRENT_VERSION) +
                 ")");
  }

  headerValid_ = true;
  log().debug << "Read file header (magic: 0x" << std::hex << header_.magic
              << std::dec << ", version: " << header_.version 
              << ", blocks: " << header_.blockCount << ")";

  return {};
}

FileStore::Roe<void> FileStore::updateHeaderBlockCount() {
  if (!isOpen()) {
    return Error("File is not open: " + filepath_);
  }

  // Seek to block count field in header
  // Position: after magic (4) + version (2) + reserved (2) = offset 8
  file_.seekp(8, std::ios::beg);

  // Write updated block count
  file_.write(reinterpret_cast<const char *>(&blockCount_), sizeof(uint64_t));

  if (!file_.good()) {
    return Error("Failed to update block count in header: " + filepath_);
  }

  // Flush to disk
  file_.flush();
  if (!file_.good()) {
    return Error("Failed to flush header update to file: " + filepath_);
  }

  header_.blockCount = blockCount_;
  log().debug << "Updated header block count to " << blockCount_;

  return {};
}

bool FileStore::hasValidHeader() const {
  return headerValid_ && header_.magic == FileHeader::MAGIC;
}

FileStore::Roe<void> FileStore::buildBlockIndex() {
  if (!isOpen()) {
    return Error("File is not open: " + filepath_);
  }

  if (!hasValidHeader()) {
    return Error("File header is not valid: " + filepath_);
  }

  blockIndex_.clear();
  
  // Start scanning from after header
  int64_t offset = getDataOffset();
  int64_t fileEnd = static_cast<int64_t>(currentSize_);
  
  while (offset + static_cast<int64_t>(SIZE_PREFIX_BYTES) <= fileEnd) {
    // Seek to current position
    file_.seekg(offset, std::ios::beg);
    if (!file_.good()) {
      log().error << "Failed to seek to offset " << offset;
      break;
    }

    // Read size prefix
    uint64_t blockSize = 0;
    file_.read(reinterpret_cast<char *>(&blockSize), SIZE_PREFIX_BYTES);
    if (file_.gcount() != static_cast<std::streamsize>(SIZE_PREFIX_BYTES)) {
      // End of file or partial read
      break;
    }

    // Validate block size
    if (offset + static_cast<int64_t>(SIZE_PREFIX_BYTES + blockSize) > fileEnd) {
      log().warning << "Block at offset " << offset 
                    << " has invalid size " << blockSize;
      break;
    }

    // Add to index
    blockIndex_.push_back(BlockEntry(offset, blockSize));
    
    // Move to next block
    offset += static_cast<int64_t>(SIZE_PREFIX_BYTES + blockSize);
  }

  indexBuilt_ = true;
  
  // Update blockCount_ from scanned data if different from header
  if (blockIndex_.size() != blockCount_) {
    log().debug << "Block count mismatch: header says " << blockCount_
                << ", scanned " << blockIndex_.size();
    blockCount_ = blockIndex_.size();
  }

  log().debug << "Built block index with " << blockIndex_.size() << " blocks";
  return {};
}

FileStore::Roe<void> FileStore::ensureBlockIndex() {
  if (indexBuilt_) {
    return {};
  }
  return buildBlockIndex();
}

// Block store interface
FileStore::Roe<std::string> FileStore::readBlock(uint64_t index) const {
  // Need to cast away const for internal operations
  FileStore* nonConstThis = const_cast<FileStore*>(this);
  
  // Ensure block index is built
  auto indexResult = nonConstThis->ensureBlockIndex();
  if (!indexResult.isOk()) {
    return Error(indexResult.error().message);
  }

  if (index >= blockIndex_.size()) {
    return Error("Block index " + std::to_string(index) + " out of range (max: " +
                 std::to_string(blockIndex_.size()) + ")");
  }

  const BlockEntry &entry = blockIndex_[index];
  
  // Allocate buffer for block data
  std::string buffer(entry.size, '\0');
  
  // Calculate data offset (skip size prefix)
  int64_t dataOffset = entry.offset + static_cast<int64_t>(SIZE_PREFIX_BYTES);

  // Clear any stream errors and seek to data position
  nonConstThis->file_.clear();
  nonConstThis->file_.seekg(dataOffset, std::ios::beg);

  if (!nonConstThis->file_.good()) {
    return Error("Failed to seek to offset " + std::to_string(dataOffset));
  }

  // Read block data
  nonConstThis->file_.read(&buffer[0], entry.size);

  int64_t bytesRead = nonConstThis->file_.gcount();

  if (bytesRead != static_cast<int64_t>(entry.size)) {
    return Error("Failed to read complete block data");
  }

  return buffer;
}

FileStore::Roe<uint64_t> FileStore::appendBlock(const std::string &block) {
  auto result = write(block.data(), block.size());
  if (!result.isOk()) {
    return Error(result.error().message);
  }
  return static_cast<uint64_t>(result.value());
}

FileStore::Roe<void> FileStore::rewindTo(uint64_t index) {
  // Ensure block index is built
  auto indexResult = ensureBlockIndex();
  if (!indexResult.isOk()) {
    return Error(indexResult.error().message);
  }

  if (index > blockCount_) {
    return Error("Cannot rewind to index " + std::to_string(index) + 
                 " (max: " + std::to_string(blockCount_) + ")");
  }

  // Truncate the file to remove blocks after the specified index
  if (index < blockCount_) {
    if (index == 0) {
      // Rewind to beginning - keep only header
      currentSize_ = HEADER_SIZE;
      blockCount_ = 0;
      blockIndex_.clear();
      
      // Truncate file to header size
      file_.close();
      std::filesystem::resize_file(filepath_, HEADER_SIZE);
      auto openResult = open();
      if (!openResult.isOk()) {
        return Error(openResult.error().message);
      }
      
      // Rewrite header with zero block count
      header_.blockCount = 0;
      auto headerResult = writeHeader();
      if (!headerResult.isOk()) {
        return Error(headerResult.error().message);
      }
    } else {
      // Rewind to specific index - keep blocks up to (but not including) index
      const BlockEntry &entry = blockIndex_[index];
      int64_t truncateOffset = entry.offset;
      
      // Update counts
      blockCount_ = index;
      currentSize_ = truncateOffset;
      
      // Truncate block index
      blockIndex_.resize(index);
      
      // Truncate file
      file_.close();
      std::filesystem::resize_file(filepath_, truncateOffset);
      auto openResult = open();
      if (!openResult.isOk()) {
        return Error(openResult.error().message);
      }
      
      // Update header
      header_.blockCount = blockCount_;
      auto headerResult = updateHeaderBlockCount();
      if (!headerResult.isOk()) {
        return Error(headerResult.error().message);
      }
    }
  }
  
  return {};
}

} // namespace pp
