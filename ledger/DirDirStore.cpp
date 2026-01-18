#include "DirDirStore.h"
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

DirDirStore::DirDirStore(const std::string &name) : DirStore(name) {}

DirDirStore::~DirDirStore() { flush(); }

DirDirStore::Roe<void> DirDirStore::init(const Config &config) {
  config_ = config;
  currentFileId_ = 0;
  currentDirId_ = 0;
  indexFilePath_ = config_.dirPath + "/idx.dat";
  fileInfoMap_.clear();
  fileIdOrder_.clear();
  dirInfoMap_.clear();
  dirIdOrder_.clear();
  totalBlockCount_ = 0;
  mode_ = Mode::FILES; // Start in FILES mode

  if (config_.maxFileSize < 1024 * 1024) {
    return Error("Max file size shall be at least 1MB");
  }

  if (config_.maxFileCount == 0) {
    return Error("Max file count must be greater than 0");
  }

  if (config_.maxDirCount == 0) {
    return Error("Max dir count must be greater than 0");
  }

  // Create directory if it doesn't exist
  std::error_code ec;
  if (!std::filesystem::exists(config_.dirPath, ec)) {
    if (ec) {
      log().error << "Failed to check directory existence " << config_.dirPath
                  << ": " << ec.message();
      return Error("Failed to check directory: " + ec.message());
    }
    if (!std::filesystem::create_directories(config_.dirPath, ec)) {
      log().error << "Failed to create directory " << config_.dirPath << ": "
                  << ec.message();
      return Error("Failed to create directory: " + ec.message());
    }
    log().info << "Created directory: " << config_.dirPath;
  } else if (ec) {
    log().error << "Failed to check directory existence " << config_.dirPath
                << ": " << ec.message();
    return Error("Failed to check directory: " + ec.message());
  }

  // Load existing index if it exists
  if (std::filesystem::exists(indexFilePath_)) {
    if (!loadIndex()) {
      log().error << "Failed to load index file";
      return Error("Failed to load index file");
    }
    log().info << "Loaded index with " << fileInfoMap_.size() << " files, "
               << dirInfoMap_.size() << " dirs, total " << totalBlockCount_
               << " blocks";

    // Find the highest IDs
    for (const auto &[fileId, _] : fileInfoMap_) {
      if (fileId > currentFileId_) {
        currentFileId_ = fileId;
      }
    }
    for (const auto &[dirId, _] : dirInfoMap_) {
      if (dirId > currentDirId_) {
        currentDirId_ = dirId;
      }
    }

    // Determine mode based on what we have
    if (!dirInfoMap_.empty()) {
      mode_ = Mode::DIRS;
    } else if (!fileInfoMap_.empty()) {
      mode_ = Mode::FILES;
    }
  } else {
    log().info << "No existing index file, starting fresh";
  }

  // Open existing block files referenced in the file info map
  for (auto &[fileId, fileInfo] : fileInfoMap_) {
    std::string filepath = getBlockFilePath(fileId);
    if (std::filesystem::exists(filepath)) {
      auto ukpBlockFile = std::make_unique<FileStore>();
      FileStore::Config bfConfig(filepath, config_.maxFileSize);
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

  // Open existing FileDirStores
  for (auto &[dirId, dirInfo] : dirInfoMap_) {
    std::string dirpath = getDirPath(dirId);
    if (std::filesystem::exists(dirpath)) {
      auto ukpFileDirStore = std::make_unique<FileDirStore>("filedirstore");
      FileDirStore::Config fdConfig;
      fdConfig.dirPath = dirpath;
      fdConfig.maxFileCount = config_.maxFileCount;
      fdConfig.maxFileSize = config_.maxFileSize;
      auto result = ukpFileDirStore->init(fdConfig);
      if (result.isOk()) {
        dirInfo.fileDirStore = std::move(ukpFileDirStore);
        log().debug << "Opened existing FileDirStore: " << dirpath
                    << " (blocks: " << dirInfo.fileDirStore->getBlockCount() << ")";
      } else {
        log().error << "Failed to open FileDirStore: " << dirpath << ": "
                    << result.error().message;
        return Error("Failed to open FileDirStore: " + dirpath + ": " +
                     result.error().message);
      }
    }
  }

  // Recalculate total block count
  totalBlockCount_ = 0;
  for (const auto &[fileId, fileInfo] : fileInfoMap_) {
    if (fileInfo.blockFile) {
      totalBlockCount_ += fileInfo.blockFile->getBlockCount();
    }
  }
  for (const auto &[dirId, dirInfo] : dirInfoMap_) {
    if (dirInfo.fileDirStore) {
      totalBlockCount_ += dirInfo.fileDirStore->getBlockCount();
    } else if (dirInfo.dirDirStore) {
      totalBlockCount_ += dirInfo.dirDirStore->getBlockCount();
    }
  }

  log().info << "DirDirStore initialized with " << fileInfoMap_.size()
             << " files, " << dirInfoMap_.size() << " dirs, and "
             << totalBlockCount_ << " blocks";

  return {};
}

bool DirDirStore::canFit(uint64_t size) const {
  // First check if the data can fit in a single file at all
  // FileStore's canFit accounts for header (24 bytes) and size prefix (8 bytes)
  // So we need to check if size would fit: maxFileSize - 24 - 8 = maxFileSize - 32
  // But to be safe, we check if size > maxFileSize, which definitely won't fit
  if (size > config_.maxFileSize) {
    return false; // Data is too large to fit in any single file
  }

  if (mode_ == Mode::FILES) {
    // Check if we can add another file
    if (fileInfoMap_.size() >= config_.maxFileCount) {
      // Need to transition to DIRS mode
      return dirInfoMap_.size() < config_.maxDirCount;
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

    // No files yet, can always create first file
    return true;
  } else if (mode_ == Mode::DIRS) {
    // Check if we can add another directory
    if (dirInfoMap_.size() >= config_.maxDirCount) {
      // Need to transition to RECURSIVE mode
      return true; // Can always create deeper
    }

    // Check if current FileDirStore can fit
    if (!dirInfoMap_.empty()) {
      auto it = dirInfoMap_.find(currentDirId_);
      if (it != dirInfoMap_.end() && it->second.fileDirStore) {
        if (it->second.fileDirStore->canFit(size)) {
          return true;
        }
        // Current FileDirStore can't fit, but we can create a new one
        return dirInfoMap_.size() < config_.maxDirCount;
      } else if (it != dirInfoMap_.end() && it->second.dirDirStore) {
        // Already in recursive mode
        return it->second.dirDirStore->canFit(size);
      }
    }

    // No dirs yet, can always create first dir
    return true;
  }

  return false;
}

uint64_t DirDirStore::getBlockCount() const { return totalBlockCount_; }

DirDirStore::Roe<std::string> DirDirStore::readBlock(uint64_t index) const {
  if (mode_ == Mode::FILES) {
    auto [fileId, indexWithinFile] = findBlockFile(index);
    if (fileId == 0 && indexWithinFile == 0 && index != 0) {
      return Error("Block " + std::to_string(index) + " not found");
    }

    auto it = fileInfoMap_.find(fileId);
    if (it == fileInfoMap_.end() || !it->second.blockFile) {
      DirDirStore *nonConstThis = const_cast<DirDirStore *>(this);
      FileStore *blockFile = nonConstThis->getBlockFile(fileId);
      if (!blockFile) {
        return Error("Block file " + std::to_string(fileId) + " not found");
      }
      it = fileInfoMap_.find(fileId);
    }

    auto readResult = it->second.blockFile->readBlock(indexWithinFile);
    if (!readResult.isOk()) {
      return Error("Failed to read block " + std::to_string(index) + ": " +
                   readResult.error().message);
    }
    return readResult.value();
  } else if (mode_ == Mode::DIRS) {
    auto [dirId, indexWithinDir] = findBlockDir(index);
    if (dirId == 0 && indexWithinDir == 0 && index != 0) {
      return Error("Block " + std::to_string(index) + " not found");
    }

    auto it = dirInfoMap_.find(dirId);
    if (it == dirInfoMap_.end()) {
      return Error("Dir " + std::to_string(dirId) + " not found");
    }

    if (it->second.fileDirStore) {
      auto readResult = it->second.fileDirStore->readBlock(indexWithinDir);
      if (!readResult.isOk()) {
        return Error("Failed to read block " + std::to_string(index) + ": " +
                     readResult.error().message);
      }
      return readResult.value();
    } else if (it->second.dirDirStore) {
      return it->second.dirDirStore->readBlock(indexWithinDir);
    }

    return Error("Dir " + std::to_string(dirId) + " has no store");
  }

  return Error("Invalid mode");
}

DirDirStore::Roe<uint64_t> DirDirStore::appendBlock(const std::string &block) {
  if (mode_ == Mode::FILES) {
    // Get active block file for writing
    FileStore *blockFile = getActiveBlockFile(block.size());
    if (!blockFile) {
      // Transition to DIRS mode
      log().info << "Reached max file count, transitioning to DIRS mode";
      mode_ = Mode::DIRS;
      currentDirId_ = 0;
      // Try again with DIRS mode
      return appendBlock(block);
    }

    // Write data to the file
    auto result = blockFile->appendBlock(block);
    if (!result.isOk()) {
      log().error << "Failed to write block to file";
      return Error("Failed to write block to file: " + result.error().message);
    }

    totalBlockCount_++;
    log().debug << "Wrote block " << totalBlockCount_ - 1 << " to file "
                << currentFileId_ << " (size: " << block.size()
                << " bytes, total blocks: " << totalBlockCount_ << ")";

    saveIndex();
    return totalBlockCount_ - 1;
  } else if (mode_ == Mode::DIRS) {
    // Get active FileDirStore or DirDirStore for writing
    BlockStore *activeStore = getActiveDirStore(block.size());
    if (!activeStore) {
      log().error << "Failed to get active dir store";
      return Error("Failed to get active dir store");
    }

    // Write data to the store
    auto result = activeStore->appendBlock(block);
    if (!result.isOk()) {
      log().error << "Failed to write block to dir store";
      return Error("Failed to write block to dir store: " +
                   result.error().message);
    }

    totalBlockCount_++;
    log().debug << "Wrote block " << totalBlockCount_ - 1 << " to dir "
                << currentDirId_ << " (size: " << block.size()
                << " bytes, total blocks: " << totalBlockCount_ << ")";

    saveIndex();
    return totalBlockCount_ - 1;
  }

  return Error("Invalid mode");
}

DirDirStore::Roe<void> DirDirStore::rewindTo(uint64_t index) {
  if (index > totalBlockCount_) {
    return Error("Cannot rewind to index " + std::to_string(index) +
                 " (max: " + std::to_string(totalBlockCount_) + ")");
  }

  if (mode_ == Mode::FILES) {
    auto [fileId, indexWithinFile] = findBlockFile(index);

    // Remove all files after the target file
    std::vector<uint32_t> filesToRemove;
    for (const auto &[fid, fileInfo] : fileInfoMap_) {
      if (fid > fileId) {
        filesToRemove.push_back(fid);
      }
    }

    for (uint32_t fid : filesToRemove) {
      fileInfoMap_.erase(fid);
      fileIdOrder_.erase(
          std::remove(fileIdOrder_.begin(), fileIdOrder_.end(), fid),
          fileIdOrder_.end());
    }

    if (fileId > 0) {
      auto it = fileInfoMap_.find(fileId);
      if (it != fileInfoMap_.end() && it->second.blockFile) {
        auto rewindResult = it->second.blockFile->rewindTo(indexWithinFile);
        if (!rewindResult.isOk()) {
          return Error("Failed to rewind file: " + rewindResult.error().message);
        }
      }
    }

    totalBlockCount_ = 0;
    for (const auto &[fid, fileInfo] : fileInfoMap_) {
      if (fileInfo.blockFile) {
        totalBlockCount_ += fileInfo.blockFile->getBlockCount();
      }
    }
  } else if (mode_ == Mode::DIRS) {
    auto [dirId, indexWithinDir] = findBlockDir(index);

    // Remove all dirs after the target dir
    std::vector<uint32_t> dirsToRemove;
    for (const auto &[did, dirInfo] : dirInfoMap_) {
      if (did > dirId) {
        dirsToRemove.push_back(did);
      }
    }

    for (uint32_t did : dirsToRemove) {
      dirInfoMap_.erase(did);
      dirIdOrder_.erase(
          std::remove(dirIdOrder_.begin(), dirIdOrder_.end(), did),
          dirIdOrder_.end());
    }

    if (dirId > 0) {
      auto it = dirInfoMap_.find(dirId);
      if (it != dirInfoMap_.end()) {
        if (it->second.fileDirStore) {
          auto rewindResult = it->second.fileDirStore->rewindTo(indexWithinDir);
          if (!rewindResult.isOk()) {
            return Error("Failed to rewind FileDirStore: " +
                         rewindResult.error().message);
          }
        } else if (it->second.dirDirStore) {
          auto rewindResult = it->second.dirDirStore->rewindTo(indexWithinDir);
          if (!rewindResult.isOk()) {
            return Error("Failed to rewind DirDirStore: " +
                         rewindResult.error().message);
          }
        }
      }
    }

    totalBlockCount_ = 0;
    for (const auto &[did, dirInfo] : dirInfoMap_) {
      if (dirInfo.fileDirStore) {
        totalBlockCount_ += dirInfo.fileDirStore->getBlockCount();
      } else if (dirInfo.dirDirStore) {
        totalBlockCount_ += dirInfo.dirDirStore->getBlockCount();
      }
    }
  }

  saveIndex();
  return {};
}

// Private methods

FileStore *DirDirStore::createBlockFile(uint32_t fileId, uint64_t startBlockId) {
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

FileStore *DirDirStore::getActiveBlockFile(uint64_t dataSize) {
  // Check if current file exists and can fit the data
  auto it = fileInfoMap_.find(currentFileId_);
  if (it != fileInfoMap_.end() && it->second.blockFile &&
      it->second.blockFile->canFit(dataSize)) {
    return it->second.blockFile.get();
  }

  // Check if we've reached max file count
  if (fileInfoMap_.size() >= config_.maxFileCount) {
    return nullptr; // Signal to transition to DIRS mode
  }

  // Need to create a new file
  currentFileId_++;
  return createBlockFile(currentFileId_, totalBlockCount_);
}

FileStore *DirDirStore::getBlockFile(uint32_t fileId) {
  auto it = fileInfoMap_.find(fileId);
  if (it != fileInfoMap_.end() && it->second.blockFile) {
    return it->second.blockFile.get();
  }

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

std::string DirDirStore::getBlockFilePath(uint32_t fileId) const {
  std::ostringstream oss;
  oss << config_.dirPath << "/" << std::setw(6) << std::setfill('0') << fileId
      << ".dat";
  return oss.str();
}

std::pair<uint32_t, uint64_t> DirDirStore::findBlockFile(uint64_t blockId) const {
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

  return {0, 0};
}

BlockStore *DirDirStore::getActiveDirStore(uint64_t dataSize) {
  // Check if current dir exists and can fit the data
  auto it = dirInfoMap_.find(currentDirId_);
  if (it != dirInfoMap_.end()) {
    if (it->second.fileDirStore && it->second.fileDirStore->canFit(dataSize)) {
      return it->second.fileDirStore.get();
    } else if (it->second.dirDirStore && it->second.dirDirStore->canFit(dataSize)) {
      return it->second.dirDirStore.get();
    }
  }

  // Check if we've reached max dir count
  if (dirInfoMap_.size() >= config_.maxDirCount) {
    // Need to create a recursive DirDirStore in current dir
    if (it != dirInfoMap_.end() && it->second.fileDirStore) {
      // Transition current FileDirStore to DirDirStore
      std::string dirpath = getDirPath(currentDirId_);
      auto ukpDirDirStore = std::make_unique<DirDirStore>("dirdirstore");
      DirDirStore::Config ddConfig;
      ddConfig.dirPath = dirpath;
      ddConfig.maxFileCount = config_.maxFileCount;
      ddConfig.maxFileSize = config_.maxFileSize;
      ddConfig.maxDirCount = config_.maxDirCount;
      auto result = ukpDirDirStore->init(ddConfig);
      if (!result.isOk()) {
        log().error << "Failed to create recursive DirDirStore: " << dirpath;
        return nullptr;
      }
      it->second.dirDirStore = std::move(ukpDirDirStore);
      it->second.fileDirStore.reset();
      return it->second.dirDirStore.get();
    } else {
      // Create new recursive DirDirStore
      currentDirId_++;
      return createDirDirStore(currentDirId_, totalBlockCount_);
    }
  }

  // Need to create a new FileDirStore
  currentDirId_++;
  return createFileDirStore(currentDirId_, totalBlockCount_);
}

FileDirStore *DirDirStore::createFileDirStore(uint32_t dirId, uint64_t startBlockId) {
  std::string dirpath = getDirPath(dirId);
  auto ukpFileDirStore = std::make_unique<FileDirStore>("filedirstore");

  FileDirStore::Config fdConfig;
  fdConfig.dirPath = dirpath;
  fdConfig.maxFileCount = config_.maxFileCount;
  fdConfig.maxFileSize = config_.maxFileSize;
  auto result = ukpFileDirStore->init(fdConfig);
  if (!result.isOk()) {
    log().error << "Failed to create FileDirStore: " << dirpath;
    return nullptr;
  }

  log().info << "Created new FileDirStore: " << dirpath
             << " (startBlockId: " << startBlockId << ")";

  FileDirStore *pFileDirStore = ukpFileDirStore.get();
  DirInfo dirInfo;
  dirInfo.fileDirStore = std::move(ukpFileDirStore);
  dirInfo.startBlockId = startBlockId;
  dirInfoMap_[dirId] = std::move(dirInfo);
  dirIdOrder_.push_back(dirId);
  return pFileDirStore;
}

DirDirStore *DirDirStore::createDirDirStore(uint32_t dirId, uint64_t startBlockId) {
  std::string dirpath = getDirPath(dirId);
  auto ukpDirDirStore = std::make_unique<DirDirStore>("dirdirstore");

  DirDirStore::Config ddConfig;
  ddConfig.dirPath = dirpath;
  ddConfig.maxFileCount = config_.maxFileCount;
  ddConfig.maxFileSize = config_.maxFileSize;
  ddConfig.maxDirCount = config_.maxDirCount;
  auto result = ukpDirDirStore->init(ddConfig);
  if (!result.isOk()) {
    log().error << "Failed to create DirDirStore: " << dirpath;
    return nullptr;
  }

  log().info << "Created new recursive DirDirStore: " << dirpath
             << " (startBlockId: " << startBlockId << ")";

  DirDirStore *pDirDirStore = ukpDirDirStore.get();
  DirInfo dirInfo;
  dirInfo.dirDirStore = std::move(ukpDirDirStore);
  dirInfo.startBlockId = startBlockId;
  dirInfoMap_[dirId] = std::move(dirInfo);
  dirIdOrder_.push_back(dirId);
  return pDirDirStore;
}

std::string DirDirStore::getDirPath(uint32_t dirId) const {
  std::ostringstream oss;
  oss << config_.dirPath << "/" << std::setw(6) << std::setfill('0') << dirId;
  return oss.str();
}

std::pair<uint32_t, uint64_t> DirDirStore::findBlockDir(uint64_t blockId) const {
  for (const auto &[dirId, dirInfo] : dirInfoMap_) {
    uint64_t startBlockId = dirInfo.startBlockId;
    uint64_t blockCount = 0;

    if (dirInfo.fileDirStore) {
      blockCount = dirInfo.fileDirStore->getBlockCount();
    } else if (dirInfo.dirDirStore) {
      blockCount = dirInfo.dirDirStore->getBlockCount();
    } else {
      continue;
    }

    if (blockId >= startBlockId && blockId < startBlockId + blockCount) {
      uint64_t indexWithinDir = blockId - startBlockId;
      return {dirId, indexWithinDir};
    }
  }

  return {0, 0};
}

bool DirDirStore::loadIndex() {
  std::ifstream indexFile(indexFilePath_, std::ios::binary);
  if (!indexFile.is_open()) {
    log().error << "Failed to open index file: " << indexFilePath_;
    return false;
  }

  fileInfoMap_.clear();
  fileIdOrder_.clear();
  dirInfoMap_.clear();
  dirIdOrder_.clear();

  IndexFileHeader header;
  InputArchive headerAr(indexFile);
  headerAr &header;
  if (headerAr.failed()) {
    log().error << "Failed to read index file header";
    indexFile.close();
    return false;
  }

  // Validate header
  if (header.magic != IndexFileHeader::MAGIC) {
    log().error << "Invalid magic number in index file header: 0x" << std::hex
                << header.magic << std::dec;
    indexFile.close();
    return false;
  }

  if (header.version != IndexFileHeader::CURRENT_VERSION) {
    log().error << "Unsupported index file version " << header.version
                << " (expected: " << IndexFileHeader::CURRENT_VERSION << ")";
    indexFile.close();
    return false;
  }

  log().debug << "Read index file header (magic: 0x" << std::hex << header.magic
              << std::dec << ", version: " << header.version
              << ", files: " << header.fileCount << ", dirs: " << header.dirCount << ")";

  // Read file entries (if any)

  FileIndexEntry fileEntry;
  for (uint32_t i = 0; i < header.fileCount; ++i) {
    if (indexFile.peek() == EOF)
      break;

    InputArchive ar(indexFile);
    ar &fileEntry;
    if (ar.failed()) {
      log().warning << "Failed to read file index entry " << i;
      break;
    }

    FileInfo fileInfo;
    fileInfo.blockFile = nullptr;
    fileInfo.startBlockId = fileEntry.startBlockId;
    fileInfoMap_[fileEntry.fileId] = std::move(fileInfo);
    fileIdOrder_.push_back(fileEntry.fileId);
  }

  // Read dir entries (if any)
  DirIndexEntry dirEntry;
  for (uint32_t i = 0; i < header.dirCount; ++i) {
    if (indexFile.peek() == EOF)
      break;

    InputArchive ar(indexFile);
    ar &dirEntry;
    if (ar.failed()) {
      log().warning << "Failed to read dir index entry " << i;
      break;
    }

    DirInfo dirInfo;
    dirInfo.fileDirStore = nullptr;
    dirInfo.dirDirStore = nullptr;
    dirInfo.startBlockId = dirEntry.startBlockId;
    dirInfo.isRecursive = dirEntry.isRecursive;
    dirInfoMap_[dirEntry.dirId] = std::move(dirInfo);
    dirIdOrder_.push_back(dirEntry.dirId);
  }

  indexFile.close();
  log().debug << "Loaded " << fileInfoMap_.size() << " file entries and "
              << dirInfoMap_.size() << " dir entries from index";

  return true;
}

bool DirDirStore::saveIndex() {
  std::ofstream indexFile(indexFilePath_, std::ios::binary | std::ios::trunc);
  if (!indexFile.is_open()) {
    log().error << "Failed to open index file for writing: " << indexFilePath_;
    return false;
  }

  if (!writeIndexHeader(indexFile)) {
    log().error << "Failed to write index file header";
    indexFile.close();
    return false;
  }

  // Write file entries
  for (uint32_t fileId : fileIdOrder_) {
    auto it = fileInfoMap_.find(fileId);
    if (it == fileInfoMap_.end()) {
      continue;
    }

    FileIndexEntry entry(fileId, it->second.startBlockId);
    std::string packed = utl::binaryPack(entry);
    indexFile.write(packed.data(), packed.size());
  }

  // Write dir entries
  for (uint32_t dirId : dirIdOrder_) {
    auto it = dirInfoMap_.find(dirId);
    if (it == dirInfoMap_.end()) {
      continue;
    }

    DirIndexEntry entry(dirId, it->second.startBlockId,
                        it->second.dirDirStore != nullptr);
    std::string packed = utl::binaryPack(entry);
    indexFile.write(packed.data(), packed.size());
  }

  indexFile.close();
  log().debug << "Saved " << fileInfoMap_.size() << " file entries and "
              << dirInfoMap_.size() << " dir entries to index";

  return true;
}

bool DirDirStore::writeIndexHeader(std::ostream &os) {
  IndexFileHeader header;
  header.fileCount = static_cast<uint32_t>(fileInfoMap_.size());
  header.dirCount = static_cast<uint32_t>(dirInfoMap_.size());
  OutputArchive ar(os);
  ar &header;

  if (!os.good()) {
    log().error << "Failed to write index file header";
    return false;
  }

  log().debug << "Wrote index file header (magic: 0x" << std::hex << header.magic
              << std::dec << ", version: " << header.version
              << ", files: " << header.fileCount << ", dirs: " << header.dirCount << ")";

  return true;
}

bool DirDirStore::readIndexHeader(std::istream &is) {
  IndexFileHeader header;

  InputArchive ar(is);
  ar &header;
  if (ar.failed()) {
    log().error << "Failed to read index file header";
    return false;
  }

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
              << std::dec << ", version: " << header.version
              << ", files: " << header.fileCount << ", dirs: " << header.dirCount << ")";

  return true;
}

void DirDirStore::flush() {
  if (!saveIndex()) {
    log().error << "Failed to save index during flush";
  }
}

} // namespace pp
