#include "FileDirStore.h"
#include "FileStore.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Serialize.hpp"
#include "Logger.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pp {

FileDirStore::FileDirStore() {
  setLogger("FileDirStore");
}

FileDirStore::~FileDirStore() { flush(); }

FileDirStore::Roe<void> FileDirStore::init(const Config &config) {
  config_ = config;
  currentFileId_ = 0;
  indexFilePath_ = getIndexFilePath(config_.dirPath);
  fileInfoMap_.clear();
  fileIdOrder_.clear();
  totalBlockCount_ = 0;

  auto sizeResult = validateMinFileSize(config_.maxFileSize);
  if (!sizeResult.isOk()) {
    return sizeResult;
  }

  if (config_.maxFileCount == 0) {
    return Error("Max file count must be greater than 0");
  }

  auto dirResult = ensureDirectory(config_.dirPath);
  if (!dirResult.isOk()) {
    return dirResult;
  }

  // Load existing index if it exists
  if (std::filesystem::exists(indexFilePath_)) {
    if (!loadIndex()) {
      log().error << "Failed to load index file";
      return Error("Failed to load index file");
    }
    log().info << "Loaded index with " << fileInfoMap_.size() << " files";
    updateCurrentFileId();
  } else {
    log().info << "No existing index file, starting fresh";
  }

  auto openResult = openExistingBlockFiles();
  if (!openResult.isOk()) {
    return openResult;
  }

  recalculateTotalBlockCount();

  log().info << "FileDirStore initialized with " << fileInfoMap_.size()
             << " files and " << totalBlockCount_ << " blocks";

  return {};
}

bool FileDirStore::canFit(uint64_t size) const {
  // First check if the data can fit in a single file at all
  // FileStore's canFit accounts for header (24 bytes: 4+2+2+8+8) and size prefix (8 bytes)
  // So the maximum data size that can fit is maxFileSize - 24 - 8 = maxFileSize - 32
  // But to be safe, we check if size > maxFileSize, which definitely won't fit
  if (size > config_.maxFileSize) {
    return false; // Data is too large to fit in any single file
  }

  // Check if we can add another file
  if (fileInfoMap_.size() >= config_.maxFileCount) {
    return false;
  }

  // Check if current file can fit the data
  if (!fileInfoMap_.empty()) {
    auto it = fileInfoMap_.find(currentFileId_);
    if (it != fileInfoMap_.end() && it->second.blockFile) {
      if (it->second.blockFile->canFit(size)) {
        return true;
      }
      // Current file can't fit, but we can create a new one if under limit
      return fileInfoMap_.size() < config_.maxFileCount;
    }
  }

  // No files yet, can always create first file (if data fits)
  return true;
}

uint64_t FileDirStore::getBlockCount() const { return totalBlockCount_; }

FileDirStore::Roe<std::string> FileDirStore::readBlock(uint64_t index) const {
  auto [fileId, indexWithinFile] = findBlockFile(index);
  if (fileId == 0 && indexWithinFile == 0 && index != 0) {
    return Error("Block " + std::to_string(index) + " not found");
  }

  // Get the block file
  auto it = fileInfoMap_.find(fileId);
  if (it == fileInfoMap_.end() || !it->second.blockFile) {
    // Try to open the file
    FileDirStore *nonConstThis = const_cast<FileDirStore *>(this);
    FileStore *blockFile = nonConstThis->getBlockFile(fileId);
    if (!blockFile) {
      return Error("Block file " + std::to_string(fileId) + " not found");
    }
    it = fileInfoMap_.find(fileId);
  }

  // Read block using FileStore's index-based read
  auto readResult = it->second.blockFile->readBlock(indexWithinFile);
  if (!readResult.isOk()) {
    return Error("Failed to read block " + std::to_string(index) + ": " +
                 readResult.error().message);
  }

  return readResult.value();
}

FileDirStore::Roe<uint64_t> FileDirStore::appendBlock(const std::string &block) {
  // Get active block file for writing
  FileStore *blockFile = getActiveBlockFile(block.size());
  if (!blockFile) {
    log().error << "Failed to get active block file";
    return Error("Failed to get active block file");
  }

  // Write data to the file (FileStore handles size prefix)
  auto result = blockFile->appendBlock(block);
  if (!result.isOk()) {
    log().error << "Failed to write block to file";
    return Error("Failed to write block to file: " + result.error().message);
  }

  // Update total block count
  totalBlockCount_++;

  log().debug << "Wrote block " << totalBlockCount_ - 1 << " to file "
              << currentFileId_ << " (size: " << block.size()
              << " bytes, total blocks: " << totalBlockCount_ << ")";

  // Save index after each write (for durability)
  saveIndex();

  return totalBlockCount_ - 1;
}

FileDirStore::Roe<void> FileDirStore::rewindTo(uint64_t index) {
  if (index > totalBlockCount_) {
    return Error("Cannot rewind to index " + std::to_string(index) +
                 " (max: " + std::to_string(totalBlockCount_) + ")");
  }

  // Find which file contains the target index
  auto [fileId, indexWithinFile] = findBlockFile(index);
  
  // Remove all files after the target file
  std::vector<uint32_t> filesToRemove;
  for (const auto &[fid, fileInfo] : fileInfoMap_) {
    if (fid > fileId) {
      filesToRemove.push_back(fid);
    }
  }

  // Remove files from map and order
  for (uint32_t fid : filesToRemove) {
    fileInfoMap_.erase(fid);
    fileIdOrder_.erase(
        std::remove(fileIdOrder_.begin(), fileIdOrder_.end(), fid),
        fileIdOrder_.end());
  }

  // Rewind the target file
  if (fileId > 0) {
    auto it = fileInfoMap_.find(fileId);
    if (it != fileInfoMap_.end() && it->second.blockFile) {
      auto rewindResult = it->second.blockFile->rewindTo(indexWithinFile);
      if (!rewindResult.isOk()) {
        return Error("Failed to rewind file: " + rewindResult.error().message);
      }
    }
  }

  // Recalculate total block count
  totalBlockCount_ = 0;
  for (const auto &[fid, fileInfo] : fileInfoMap_) {
    if (fileInfo.blockFile) {
      totalBlockCount_ += fileInfo.blockFile->getBlockCount();
    }
  }

  saveIndex();
  return {};
}

// Private methods

FileStore *FileDirStore::createBlockFile(uint32_t fileId, uint64_t startBlockId) {
  std::string filepath = getBlockFilePath(fileId);
  auto ukpBlockFile = std::make_unique<FileStore>();

  FileStore::Config bfConfig(filepath, config_.maxFileSize);
  auto result = ukpBlockFile->init(bfConfig);
  if (!result.isOk()) {
    log().error << "Failed to create block file: " << filepath;
    return nullptr;
  }

  log().info << "Created new block file: " << filepath
             << " (startBlockId: " << startBlockId << ")";

  FileStore *pBlockFile = ukpBlockFile.get();
  FileInfo fileInfo;
  fileInfo.blockFile = std::move(ukpBlockFile);
  fileInfo.startBlockId = startBlockId;
  fileInfoMap_[fileId] = std::move(fileInfo);
  fileIdOrder_.push_back(fileId);
  return pBlockFile;
}

FileStore *FileDirStore::getActiveBlockFile(uint64_t dataSize) {
  // Check if current file exists and can fit the data
  auto it = fileInfoMap_.find(currentFileId_);
  if (it != fileInfoMap_.end() && it->second.blockFile &&
      it->second.blockFile->canFit(dataSize)) {
    return it->second.blockFile.get();
  }

  // Check if we've reached max file count
  if (fileInfoMap_.size() >= config_.maxFileCount) {
    log().error << "Reached max file count: " << config_.maxFileCount;
    return nullptr;
  }

  // Need to create a new file
  currentFileId_++;
  return createBlockFile(currentFileId_, totalBlockCount_);
}

FileStore *FileDirStore::getBlockFile(uint32_t fileId) {
  auto it = fileInfoMap_.find(fileId);
  if (it != fileInfoMap_.end() && it->second.blockFile) {
    return it->second.blockFile.get();
  }

  // Try to open the file if it exists
  std::string filepath = getBlockFilePath(fileId);
  if (std::filesystem::exists(filepath)) {
    auto ukpBlockFile = std::make_unique<FileStore>();
    FileStore::Config bfConfig(filepath, config_.maxFileSize);
    auto result = ukpBlockFile->init(bfConfig);
    if (result.isOk()) {
      FileStore *pBlockFile = ukpBlockFile.get();
      if (fileInfoMap_.find(fileId) == fileInfoMap_.end()) {
        FileInfo fileInfo;
        fileInfo.blockFile = std::move(ukpBlockFile);
        fileInfo.startBlockId = 0; // Will be loaded from index
        fileInfoMap_[fileId] = std::move(fileInfo);
      } else {
        fileInfoMap_[fileId].blockFile = std::move(ukpBlockFile);
      }
      return pBlockFile;
    }
  }

  return nullptr;
}

std::string FileDirStore::getBlockFilePath(uint32_t fileId) const {
  return config_.dirPath + "/" + formatId(fileId) + ".dat";
}

std::pair<uint32_t, uint64_t> FileDirStore::findBlockFile(uint64_t blockId) const {
  // Find the file containing this blockId
  for (const auto &[fileId, fileInfo] : fileInfoMap_) {
    uint64_t startBlockId = fileInfo.startBlockId;
    uint64_t blockCount = 0;

    if (fileInfo.blockFile) {
      blockCount = fileInfo.blockFile->getBlockCount();
    } else {
      continue;
    }

    if (blockId >= startBlockId && blockId < startBlockId + blockCount) {
      uint64_t indexWithinFile = blockId - startBlockId;
      return {fileId, indexWithinFile};
    }
  }

  return {0, 0}; // Not found
}

bool FileDirStore::loadIndex() {
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

  // Read index entries
  FileIndexEntry entry;
  while (indexFile.good() && !indexFile.eof()) {
    if (indexFile.peek() == EOF)
      break;

    InputArchive ar(indexFile);
    ar &entry;
    if (ar.failed()) {
      if (indexFile.gcount() == 0)
        break;
      log().warning << "Failed to read complete index entry";
      break;
    }

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

bool FileDirStore::saveIndex() {
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

  // Write index entries
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

bool FileDirStore::writeIndexHeader(std::ostream &os) {
  IndexFileHeader header;
  OutputArchive ar(os);
  ar &header;

  if (!os.good()) {
    log().error << "Failed to write index file header";
    return false;
  }

  log().debug << "Wrote index file header (magic: 0x" << std::hex << header.magic
              << std::dec << ", version: " << header.version << ")";

  return true;
}

bool FileDirStore::readIndexHeader(std::istream &is) {
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

void FileDirStore::flush() {
  // Save index
  if (!saveIndex()) {
    log().error << "Failed to save index during flush";
  }
}

FileDirStore::Roe<std::string> FileDirStore::relocateToSubdir(const std::string &subdirName) {
  log().info << "Relocating FileDirStore contents to subdirectory: " << subdirName;

  // Close all open files first
  for (auto &[fileId, fileInfo] : fileInfoMap_) {
    fileInfo.blockFile.reset();
  }

  if (!saveIndex()) {
    return Error("Failed to save index before relocation");
  }

  std::string originalPath = config_.dirPath;
  auto relocateResult = performDirectoryRelocation(originalPath, subdirName);
  if (!relocateResult.isOk()) {
    return relocateResult;
  }
  std::string targetSubdir = relocateResult.value();

  config_.dirPath = targetSubdir;
  indexFilePath_ = getIndexFilePath(targetSubdir);

  auto reopenResult = reopenBlockFiles();
  if (!reopenResult.isOk()) {
    return Error(reopenResult.error().message);
  }

  log().info << "Successfully relocated FileDirStore to: " << targetSubdir;
  return targetSubdir;
}

// Helper methods

FileDirStore::Roe<void> FileDirStore::openExistingBlockFiles() {
  for (auto &[fileId, fileInfo] : fileInfoMap_) {
    std::string filepath = getBlockFilePath(fileId);
    if (!std::filesystem::exists(filepath)) {
      continue;
    }

    auto ukpBlockFile = std::make_unique<FileStore>();
    FileStore::Config bfConfig(filepath, config_.maxFileSize);
    auto result = ukpBlockFile->init(bfConfig);
    if (!result.isOk()) {
      log().error << "Failed to open block file: " << filepath << ": "
                  << result.error().message;
      return Error("Failed to open block file: " + filepath + ": " +
                   result.error().message);
    }

    fileInfo.blockFile = std::move(ukpBlockFile);
    log().debug << "Opened existing block file: " << filepath
                << " (blocks: " << fileInfo.blockFile->getBlockCount() << ")";
  }
  return {};
}

FileDirStore::Roe<void> FileDirStore::reopenBlockFiles() {
  for (auto &[fileId, fileInfo] : fileInfoMap_) {
    std::string filepath = getBlockFilePath(fileId);
    if (!std::filesystem::exists(filepath)) {
      continue;
    }

    auto ukpBlockFile = std::make_unique<FileStore>();
    FileStore::Config bfConfig(filepath, config_.maxFileSize);
    auto result = ukpBlockFile->init(bfConfig);
    if (!result.isOk()) {
      log().error << "Failed to reopen block file: " << filepath;
      return Error("Failed to reopen block file: " + result.error().message);
    }

    fileInfo.blockFile = std::move(ukpBlockFile);
    log().debug << "Reopened block file: " << filepath;
  }
  return {};
}

void FileDirStore::recalculateTotalBlockCount() {
  totalBlockCount_ = 0;
  for (const auto &[fileId, fileInfo] : fileInfoMap_) {
    if (fileInfo.blockFile) {
      totalBlockCount_ += fileInfo.blockFile->getBlockCount();
    }
  }
}

void FileDirStore::updateCurrentFileId() {
  for (const auto &[fileId, _] : fileInfoMap_) {
    if (fileId > currentFileId_) {
      currentFileId_ = fileId;
    }
  }
}

} // namespace pp
