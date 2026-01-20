#include "BlockDir.h"
#include "../lib/BinaryPack.hpp"
#include "Logger.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pp {

BlockDir::BlockDir() {
  setLogger("BlockDir");
}

BlockDir::~BlockDir() { flush(); }

BlockDir::Roe<void> BlockDir::init(const Config &config,
                                   bool manageBlockchain) {
  dirPath_ = config.dirPath;
  maxFileSize_ = config.maxFileSize;
  currentFileId_ = 0;
  indexFilePath_ = dirPath_ + "/idx.dat";
  fileInfoMap_.clear();
  fileIdOrder_.clear();
  totalBlockCount_ = 0;
  managesBlockchain_ = manageBlockchain;

  // Initialize blockchain if this BlockDir manages it
  if (managesBlockchain_) {
    ukpBlockchain_ = std::make_unique<BlockChain>();
  }

  // Create directory if it doesn't exist
  std::error_code ec;
  if (!std::filesystem::exists(dirPath_, ec)) {
    if (ec) {
      log().error << "Failed to check directory existence " << dirPath_ << ": "
                  << ec.message();
      return Error("Failed to check directory: " + ec.message());
    }
    if (!std::filesystem::create_directories(dirPath_, ec)) {
      log().error << "Failed to create directory " << dirPath_ << ": "
                  << ec.message();
      return Error("Failed to create directory: " + ec.message());
    }
    log().info << "Created block directory: " << dirPath_;
  } else if (ec) {
    log().error << "Failed to check directory existence " << dirPath_ << ": "
                << ec.message();
    return Error("Failed to check directory: " + ec.message());
  }

  // Load existing index if it exists
  if (std::filesystem::exists(indexFilePath_)) {
    if (!loadIndex()) {
      log().error << "Failed to load index file";
      return Error("Failed to load index file");
    }
    log().info << "Loaded index with " << fileInfoMap_.size()
               << " files, total " << totalBlockCount_ << " blocks";

    // Find the highest file ID from the file info map
    for (const auto &[fileId, _] : fileInfoMap_) {
      if (fileId > currentFileId_) {
        currentFileId_ = fileId;
      }
    }
  } else {
    log().info << "No existing index file, starting fresh";
  }

  // Open existing block files referenced in the file info map
  for (auto &[fileId, fileInfo] : fileInfoMap_) {
    std::string filepath = getBlockFilePath(fileId);
    if (std::filesystem::exists(filepath)) {
      auto ukpBlockFile = std::make_unique<FileStore>();
      FileStore::Config bfConfig(filepath, maxFileSize_);
      auto result = ukpBlockFile->init(bfConfig);
      if (result.isOk()) {
        fileInfo.blockFile = std::move(ukpBlockFile);
        log().debug << "Opened existing block file: " << filepath 
                    << " (blocks: " << fileInfo.blockFile->getBlockCount() << ")";
      } else {
        log().error << "Failed to open block file: " << filepath << ": "
                    << result.error().message;
        return Error("Failed to open block file: " + filepath + ": " +
                     result.error().message);
      }
    }
  }

  // Recalculate total block count from opened files
  totalBlockCount_ = 0;
  for (const auto &[fileId, fileInfo] : fileInfoMap_) {
    if (fileInfo.blockFile) {
      totalBlockCount_ += fileInfo.blockFile->getBlockCount();
    }
  }

  log().info << "BlockDir initialized with " << fileInfoMap_.size()
             << " files and " << totalBlockCount_ << " blocks";

  // Populate blockchain from existing blocks if managing blockchain
  if (managesBlockchain_ && ukpBlockchain_ && totalBlockCount_ > 0) {
    auto result = populateBlockchainFromStorage();
    if (result.isError()) {
      log().error << "Failed to populate blockchain from storage: "
                  << result.error().message;
      return result.error();
    } else {
      log().info << "Populated blockchain with " << ukpBlockchain_->getSize()
                 << " blocks from storage";
    }
  }

  return {};
}

BlockDir::Roe<void> BlockDir::writeBlock(uint64_t blockId, const void *data,
                                         uint64_t size) {
  // Check if block already exists
  if (hasBlock(blockId)) {
    log().warning << "Block " << blockId
                  << " already exists, overwriting not supported";
    return Error("Block already exists");
  }

  // Get active block file for writing
  FileStore *blockFile = getActiveBlockFile(size);
  if (!blockFile) {
    log().error << "Failed to get active block file";
    return Error("Failed to get active block file");
  }

  // Write data to the file (FileStore handles size prefix)
  auto result = blockFile->write(data, size);
  if (!result.isOk()) {
    log().error << "Failed to write block " << blockId << " to file";
    return Error("Failed to write block to file: " + result.error().message);
  }

  // Update total block count
  totalBlockCount_++;

  log().debug << "Wrote block " << blockId << " to file " << currentFileId_
              << " (size: " << size << " bytes, total blocks: " << totalBlockCount_ << ")";

  // Save index after each write (for durability)
  saveIndex();

  return {};
}

BlockDir::Roe<int64_t> BlockDir::readBlock(uint64_t blockId, void *data,
                                           size_t maxSize) const {
  auto [fileId, indexWithinFile] = findBlockFile(blockId);
  if (fileId == 0 && indexWithinFile == 0 && blockId != 0) {
    // Block not found (special case: blockId 0 might be valid)
    auto it = fileInfoMap_.find(fileId);
    if (it == fileInfoMap_.end()) {
      return Error("Block " + std::to_string(blockId) + " not found");
    }
  }

  // Get the block file
  auto it = fileInfoMap_.find(fileId);
  if (it == fileInfoMap_.end() || !it->second.blockFile) {
    return Error("Block file " + std::to_string(fileId) + " not found or not open");
  }

  // Read block using FileStore's index-based read
  auto readResult = it->second.blockFile->readBlock(indexWithinFile, data, maxSize);
  if (!readResult.isOk()) {
    return Error("Failed to read block " + std::to_string(blockId) + ": " +
                 readResult.error().message);
  }

  return readResult.value();
}

bool BlockDir::hasBlock(uint64_t blockId) const {
  auto [fileId, indexWithinFile] = findBlockFile(blockId);
  if (fileId == 0 && blockId != 0) {
    return false;
  }
  // Check if file exists and has the block
  auto it = fileInfoMap_.find(fileId);
  if (it == fileInfoMap_.end()) {
    return false;
  }
  // Block exists if blockId is within the range for this file
  return true;
}

std::pair<uint32_t, uint64_t> BlockDir::findBlockFile(uint64_t blockId) const {
  // Find the file containing this blockId
  // Block indices are sequential, so we find the file where:
  // startBlockId <= blockId < startBlockId + blockCount
  
  for (const auto &[fileId, fileInfo] : fileInfoMap_) {
    uint64_t startBlockId = fileInfo.startBlockId;
    uint64_t blockCount = 0;
    
    if (fileInfo.blockFile) {
      blockCount = fileInfo.blockFile->getBlockCount();
    } else {
      // File not open, skip for now
      continue;
    }
    
    if (blockId >= startBlockId && blockId < startBlockId + blockCount) {
      uint64_t indexWithinFile = blockId - startBlockId;
      return {fileId, indexWithinFile};
    }
  }
  
  return {0, 0}; // Not found
}

void BlockDir::flush() {
  // FileStore now flushes automatically after each write operation,
  // so no explicit flush needed here. Just save the index.

  // Save index
  if (!saveIndex()) {
    log().error << "Failed to save index during flush";
  }
}

FileStore *BlockDir::createBlockFile(uint32_t fileId, uint64_t startBlockId) {
  std::string filepath = getBlockFilePath(fileId);
  auto ukpBlockFile = std::make_unique<FileStore>();

  FileStore::Config bfConfig(filepath, maxFileSize_);
  auto result = ukpBlockFile->init(bfConfig);
  if (!result.isOk()) {
    log().error << "Failed to create block file: " << filepath;
    return nullptr;
  }

  log().info << "Created new block file: " << filepath 
             << " (startBlockId: " << startBlockId << ")";

  FileStore *pBlockFile = ukpBlockFile.get();
  // Create FileInfo with the starting block ID
  FileInfo fileInfo;
  fileInfo.blockFile = std::move(ukpBlockFile);
  fileInfo.startBlockId = startBlockId;
  fileInfoMap_[fileId] = std::move(fileInfo);
  fileIdOrder_.push_back(fileId); // Track file creation order
  return pBlockFile;
}

FileStore *BlockDir::getActiveBlockFile(uint64_t dataSize) {
  // Check if current file exists and can fit the data
  auto it = fileInfoMap_.find(currentFileId_);
  if (it != fileInfoMap_.end() && it->second.blockFile &&
      it->second.blockFile->canFit(dataSize)) {
    return it->second.blockFile.get();
  }

  // Need to create a new file
  currentFileId_++;
  // New file starts where the previous files left off
  return createBlockFile(currentFileId_, totalBlockCount_);
}

FileStore *BlockDir::getBlockFile(uint32_t fileId) {
  auto it = fileInfoMap_.find(fileId);
  if (it != fileInfoMap_.end() && it->second.blockFile) {
    return it->second.blockFile.get();
  }

  // Try to open the file if it exists
  std::string filepath = getBlockFilePath(fileId);
  if (std::filesystem::exists(filepath)) {
    auto ukpBlockFile = std::make_unique<FileStore>();
    FileStore::Config bfConfig(filepath, maxFileSize_);
    auto result = ukpBlockFile->init(bfConfig);
    if (result.isOk()) {
      FileStore *pBlockFile = ukpBlockFile.get();
      // If not in map, create entry with startBlockId 0 (will be updated from index)
      if (fileInfoMap_.find(fileId) == fileInfoMap_.end()) {
        FileInfo fileInfo;
        fileInfo.blockFile = std::move(ukpBlockFile);
        fileInfo.startBlockId = 0;
        fileInfoMap_[fileId] = std::move(fileInfo);
      } else {
        fileInfoMap_[fileId].blockFile = std::move(ukpBlockFile);
      }
      return pBlockFile;
    }
  }

  return nullptr;
}

const FileStore *BlockDir::getBlockFileConst(uint32_t fileId) const {
  auto it = fileInfoMap_.find(fileId);
  if (it != fileInfoMap_.end() && it->second.blockFile) {
    return it->second.blockFile.get();
  }
  return nullptr;
}

std::string BlockDir::getBlockFilePath(uint32_t fileId) const {
  std::ostringstream oss;
  oss << dirPath_ << "/" << std::setw(6) << std::setfill('0') << fileId
      << ".dat";
  return oss.str();
}

bool BlockDir::loadIndex() {
  std::ifstream indexFile(indexFilePath_, std::ios::binary);
  if (!indexFile.is_open()) {
    log().error << "Failed to open index file: " << indexFilePath_;
    return false;
  }

  fileInfoMap_.clear();
  fileIdOrder_.clear();

  // Read and validate header
  if (!readIndexHeader(indexFile)) {
    log().error << "Failed to read or validate index file header";
    indexFile.close();
    return false;
  }

  // Read index entries (simplified: fileId + startBlockId)
  FileIndexEntry entry;
  while (indexFile.good() && !indexFile.eof()) {
    // Check if we're at end of file
    if (indexFile.peek() == EOF)
      break;

    // Deserialize using InputArchive
    InputArchive ar(indexFile);
    ar &entry;
    if (ar.failed()) {
      if (indexFile.gcount() == 0)
        break; // End of file
      log().warning << "Failed to read complete index entry";
      break;
    }

    // Create FileInfo with empty FileStore (will be loaded when needed)
    FileInfo fileInfo;
    fileInfo.blockFile = nullptr;
    fileInfo.startBlockId = entry.startBlockId;
    fileInfoMap_[entry.fileId] = std::move(fileInfo);
    fileIdOrder_.push_back(entry.fileId);
  }

  indexFile.close();
  log().debug << "Loaded " << fileInfoMap_.size() << " file entries from index";

  return true;
}

bool BlockDir::saveIndex() {
  std::ofstream indexFile(indexFilePath_, std::ios::binary | std::ios::trunc);
  if (!indexFile.is_open()) {
    log().error << "Failed to open index file for writing: " << indexFilePath_;
    return false;
  }

  // Write header first
  if (!writeIndexHeader(indexFile)) {
    log().error << "Failed to write index file header";
    indexFile.close();
    return false;
  }

  // Write index entries (simplified: fileId + startBlockId)
  // Write in order of fileIdOrder_ to maintain file order
  for (uint32_t fileId : fileIdOrder_) {
    auto it = fileInfoMap_.find(fileId);
    if (it == fileInfoMap_.end()) {
      continue;
    }
    
    FileIndexEntry entry(fileId, it->second.startBlockId);
    std::string packed = utl::binaryPack(entry);
    indexFile.write(packed.data(), packed.size());
  }

  indexFile.close();
  log().debug << "Saved " << fileInfoMap_.size() << " file entries to index";

  return true;
}

bool BlockDir::writeIndexHeader(std::ostream &os) {
  IndexFileHeader header;
  OutputArchive ar(os);
  ar &header;

  if (!os.good()) {
    log().error << "Failed to write index file header";
    return false;
  }

  log().debug << "Wrote index file header (magic: 0x" << std::hex
              << header.magic << std::dec << ", version: " << header.version
              << ")";

  return true;
}

bool BlockDir::readIndexHeader(std::istream &is) {
  IndexFileHeader header;

  InputArchive ar(is);
  ar &header;
  if (ar.failed()) {
    log().error << "Failed to read index file header";
    return false;
  }

  // Validate header
  if (header.magic != IndexFileHeader::MAGIC) {
    log().error << "Invalid magic number in index file header: 0x" << std::hex
                << header.magic << std::dec;
    return false;
  }

  if (header.version != IndexFileHeader::CURRENT_VERSION) {
    log().error << "Unsupported index file version " << header.version
                << " (expected: " << IndexFileHeader::CURRENT_VERSION << ")";
    return false;
  }

  log().debug << "Read index file header (magic: 0x" << std::hex << header.magic
              << std::dec << ", version: " << header.version << ")";

  return true;
}

uint32_t BlockDir::getFrontFileId() const {
  if (fileIdOrder_.empty()) {
    return 0;
  }
  return fileIdOrder_.front();
}

std::unique_ptr<FileStore> BlockDir::popFrontFile() {
  if (fileIdOrder_.empty()) {
    log().warning << "No files to pop from BlockDir";
    return nullptr;
  }

  uint32_t frontFileId = fileIdOrder_.front();
  fileIdOrder_.erase(fileIdOrder_.begin());

  auto it = fileInfoMap_.find(frontFileId);
  if (it == fileInfoMap_.end() || !it->second.blockFile) {
    log().error << "Front file ID " << frontFileId << " not found in file map";
    return nullptr;
  }

  // Get block count from the file being popped
  uint64_t blockCount = it->second.blockFile->getBlockCount();

  // Extract and return the file, remove from fileInfoMap_
  std::unique_ptr<FileStore> poppedFile = std::move(it->second.blockFile);
  fileInfoMap_.erase(it);

  // Update total block count
  if (totalBlockCount_ >= blockCount) {
    totalBlockCount_ -= blockCount;
  }

  log().info << "Popped front file " << frontFileId << " with "
             << blockCount << " blocks";
  return poppedFile;
}

BlockDir::Roe<void> BlockDir::moveFrontFileTo(BlockDir &targetDir) {
  uint32_t frontFileId = getFrontFileId();
  if (frontFileId == 0) {
    return Error("No files to move");
  }

  // Get file info from fileInfoMap_ before popping
  auto fileInfoIt = fileInfoMap_.find(frontFileId);
  if (fileInfoIt == fileInfoMap_.end()) {
    return Error("Front file not found in fileInfoMap_");
  }

  const FileInfo &fileInfo = fileInfoIt->second;
  uint64_t startBlockId = fileInfo.startBlockId;
  
  // Get block count from the file
  uint64_t blockCount = 0;
  if (fileInfo.blockFile) {
    blockCount = fileInfo.blockFile->getBlockCount();
  }

  // Create FileInfo in target directory (FileStore will be loaded when needed)
  if (targetDir.fileInfoMap_.find(frontFileId) ==
      targetDir.fileInfoMap_.end()) {
    FileInfo targetFileInfo;
    targetFileInfo.blockFile = nullptr;
    targetFileInfo.startBlockId = startBlockId;
    targetDir.fileInfoMap_[frontFileId] = std::move(targetFileInfo);
  } else {
    return Error("Front file already exists in target directory");
  }

  std::string sourceFilePath = getBlockFilePath(frontFileId);
  std::string targetFilePath = targetDir.getBlockFilePath(frontFileId);

  // Pop the file from this directory
  auto poppedFile = popFrontFile();
  if (!poppedFile) {
    return Error("Failed to pop front file");
  }

  // Release the file handle before moving
  poppedFile.reset();

  // Move file on disk
  std::error_code ec;
  std::filesystem::rename(sourceFilePath, targetFilePath, ec);
  if (ec) {
    return Error("Failed to move file: " + ec.message());
  }

  // Update target directory's file tracking if needed
  // Add file ID to target's tracking if not already present
  auto it = std::find(targetDir.fileIdOrder_.begin(),
                      targetDir.fileIdOrder_.end(), frontFileId);
  if (it == targetDir.fileIdOrder_.end()) {
    targetDir.fileIdOrder_.push_back(frontFileId);
  }

  // Update target's total block count
  targetDir.totalBlockCount_ += blockCount;

  // Automatically trim blocks from blockchain if this BlockDir manages a
  // blockchain
  if (managesBlockchain_ && ukpBlockchain_) {
    size_t removed = trimBlocks(blockCount);
    if (removed > 0) {
      log().info << "Automatically trimmed " << removed
                 << " blocks from blockchain after moving to archive";
    }
  }

  // Save both indexes
  saveIndex();
  targetDir.saveIndex();

  log().info << "Moved front file " << frontFileId << " with " << blockCount
             << " blocks to target directory";
  return {};
}

size_t BlockDir::getTotalStorageSize() const {
  size_t totalSize = 0;
  for (const auto &[fileId, fileInfo] : fileInfoMap_) {
    if (fileInfo.blockFile) {
      // File is open, use the open file's size
      totalSize += fileInfo.blockFile->getCurrentSize();
    } else {
      // File is not open, check file size on disk
      std::string filepath = getBlockFilePath(fileId);
      std::error_code ec;
      if (std::filesystem::exists(filepath, ec) && !ec) {
        uintmax_t fileSize = std::filesystem::file_size(filepath, ec);
        if (!ec) {
          totalSize += fileSize;
        }
        // Skip this file if we can't get its size
        // (file might have been deleted or is inaccessible)
      }
    }
  }
  return totalSize;
}

// Blockchain management methods
bool BlockDir::addBlock(std::shared_ptr<Block> block) {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return false;
  }

  // Assume block index is always increment of 1 from last block
  // Set the block index to the current blockchain size (which will be the next
  // index)
  uint64_t nextIndex = ukpBlockchain_->getSize();
  block->setIndex(nextIndex);

  // Add block to in-memory blockchain
  if (!ukpBlockchain_->addBlock(block)) {
    return false;
  }

  // Automatically write block to storage
  // Using block index as the block ID for storage
  // Serialize the full block using ltsToString() for long-term storage
  std::string blockData = block->ltsToString();
  auto writeResult =
      writeBlock(block->getIndex(), blockData.data(), blockData.size());
  if (!writeResult.isOk()) {
    log().error << "Failed to write block " << block->getIndex()
                << " to storage: " << writeResult.error().message;
    // Note: We don't rollback the blockchain addition, as the block is already
    // in memory In a production system, you might want to handle this
    // differently
    return false;
  }

  // Automatically flush after adding block
  flush();

  return true;
}

std::shared_ptr<Block> BlockDir::getLatestBlock() const {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return nullptr;
  }
  return ukpBlockchain_->getLatestConcreteBlock();
}

size_t BlockDir::getBlockchainSize() const {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return 0;
  }
  return ukpBlockchain_->getSize();
}

std::shared_ptr<Block> BlockDir::getBlock(uint64_t index) const {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return nullptr;
  }
  return ukpBlockchain_->getBlock(index);
}

bool BlockDir::isBlockchainValid() const {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return false;
  }
  return ukpBlockchain_->isValid();
}

std::string BlockDir::getLastBlockHash() const {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return "0";
  }
  return ukpBlockchain_->getLastBlockHash();
}

size_t BlockDir::trimBlocks(size_t count) {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return 0;
  }
  return ukpBlockchain_->trimBlocks(count);
}

BlockDir::Roe<size_t> BlockDir::loadBlocksFromFile(uint32_t fileId) {
  auto it = fileInfoMap_.find(fileId);
  if (it == fileInfoMap_.end()) {
    return Error("File " + std::to_string(fileId) + " not found in fileInfoMap_");
  }
  
  const FileInfo &fileInfo = it->second;
  uint64_t startBlockId = fileInfo.startBlockId;

  // Get the block file
  FileStore *blockFile = getBlockFile(fileId);
  if (!blockFile) {
    log().error << "Failed to get block file " << fileId;
    return Error("Failed to get block file " + std::to_string(fileId));
  }

  // Get block count from the file
  uint64_t blockCount = blockFile->getBlockCount();
  
  size_t loadedCount = 0;

  // Iterate through blocks using FileStore's readBlock method
  for (uint64_t i = 0; i < blockCount; i++) {
    uint64_t blockId = startBlockId + i;
    
    // First get the block size
    auto sizeResult = blockFile->getBlockSize(i);
    if (!sizeResult.isOk()) {
      log().error << "Failed to get size of block " << i << " in file " << fileId;
      return Error("Failed to get block size: " + sizeResult.error().message);
    }
    
    uint64_t blockSize = sizeResult.value();
    
    // Read block data using FileStore's index-based read
    std::string blockData;
    blockData.resize(static_cast<size_t>(blockSize));
    auto readResult = blockFile->readBlock(i, &blockData[0], blockData.size());
    if (!readResult.isOk()) {
      log().error << "Failed to read block " << blockId << " from file " << fileId;
      return Error("Failed to read block " + std::to_string(blockId) +
                   " from file " + std::to_string(fileId) + ": " +
                   readResult.error().message);
    }

    // Deserialize block from binary format
    auto block = std::make_shared<Block>();
    if (!block->ltsFromString(blockData)) {
      log().error << "Failed to deserialize block " << blockId
                  << " from storage";
      return Error("Failed to deserialize block " + std::to_string(blockId) +
                   " from storage");
    }

    // Verify block index matches blockId
    if (block->getIndex() != blockId) {
      log().warning << "Block index mismatch: expected " << blockId << ", got "
                    << block->getIndex();
      return Error("Block index mismatch: expected " + std::to_string(blockId) +
                   ", got " + std::to_string(block->getIndex()));
    }

    // Add to blockchain
    // Note: We don't use addBlock() here because it would try to write to
    // storage again Instead, we directly add to the blockchain
    if (!ukpBlockchain_->addBlock(block)) {
      log().error << "Failed to add block " << blockId << " to blockchain";
      return Error("Failed to add block " + std::to_string(blockId) +
                   " to blockchain");
    }

    loadedCount++;
  }

  return loadedCount;
}

BlockDir::Roe<void> BlockDir::populateBlockchainFromStorage() {
  if (!managesBlockchain_ || !ukpBlockchain_) {
    return Error(
        "Blockchain management not enabled or blockchain not initialized");
  }

  // Iterate through files and their starting block IDs
  // Collect file IDs and sort them by startBlockId to maintain sequential order
  std::vector<std::pair<uint32_t, uint64_t>> fileIdWithStart;
  for (const auto &[fileId, fileInfo] : fileInfoMap_) {
    fileIdWithStart.push_back(
        std::make_pair(fileId, fileInfo.startBlockId));
  }
  // Sort by startBlockId to process files in sequential order
  std::sort(fileIdWithStart.begin(), fileIdWithStart.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });

  std::vector<uint32_t> fileIds;
  for (const auto &[fileId, _] : fileIdWithStart) {
    fileIds.push_back(fileId);
  }

  size_t loadedCount = 0;

  // Process each file in order
  for (uint32_t fileId : fileIds) {
    auto result = loadBlocksFromFile(fileId);
    if (!result.isOk()) {
      log().error << "Failed to load blocks from file " << fileId << ": "
                  << result.error().message;
      return result.error();
    }
    loadedCount += result.value();
  }

  log().debug << "Loaded " << loadedCount
              << " blocks from storage into blockchain";
  if (loadedCount == 0) {
    return Error("No blocks were loaded from storage");
  }

  return {};
}

} // namespace pp
