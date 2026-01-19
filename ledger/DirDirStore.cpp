#include "DirDirStore.h"
#include "FileDirStore.h"
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
  currentDirId_ = 0;
  indexFilePath_ = config_.dirPath + "/idx.dat";
  rootStore_.reset();
  dirInfoMap_.clear();
  dirIdOrder_.clear();
  totalBlockCount_ = 0;

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
  bool useRootStore = true;  // Default to root store mode
  if (std::filesystem::exists(indexFilePath_)) {
    // Check the magic number to determine if this is a DirDirStore index
    // or a FileDirStore index (meaning we're in root store mode)
    std::ifstream checkFile(indexFilePath_, std::ios::binary);
    if (checkFile.is_open()) {
      // Use archive to read magic with proper serialization
      uint32_t magic = 0;
      InputArchive ar(checkFile);
      ar &magic;
      checkFile.close();
      
      if (ar.failed()) {
        log().error << "Failed to read magic from index file";
        return Error("Failed to read magic from index file");
      }
      
      if (magic == MAGIC_DIR_DIR) {
        // This is a DirDirStore index with subdirectories
        if (!loadIndex()) {
          log().error << "Failed to load index file";
          return Error("Failed to load index file");
        }
        log().info << "Loaded index with " << dirInfoMap_.size() << " dirs";

        // Find the highest dir ID
        for (const auto &[dirId, _] : dirInfoMap_) {
          if (dirId > currentDirId_) {
            currentDirId_ = dirId;
          }
        }
        useRootStore = false;
      } else if (magic == MAGIC_FILE_DIR) {
        // This is a FileDirStore index - use root store mode
        log().info << "Found FileDirStore index, using root store mode";
        useRootStore = true;
      } else {
        log().error << "Unknown magic number in index file: 0x" << std::hex << magic << std::dec;
        return Error("Unknown magic number in index file");
      }
    }
  } else {
    log().info << "No existing index file, starting fresh";
  }

  // Initialize based on mode
  if (useRootStore) {
    // Initialize rootStore_ to manage files at root level
    rootStore_ = std::make_unique<FileDirStore>("root-filedirstore");
    FileDirStore::Config fdConfig;
    fdConfig.dirPath = config_.dirPath;
    fdConfig.maxFileCount = config_.maxFileCount;
    fdConfig.maxFileSize = config_.maxFileSize;
    auto result = rootStore_->init(fdConfig);
    if (!result.isOk()) {
      log().error << "Failed to initialize root FileDirStore: " 
                  << result.error().message;
      return Error("Failed to initialize root FileDirStore: " + 
                   result.error().message);
    }
    
    totalBlockCount_ = rootStore_->getBlockCount();
    log().info << "Initialized root FileDirStore with " << totalBlockCount_ << " blocks";
  } else {
    // Open existing FileDirStores/DirDirStores in subdirectories
    for (auto &[dirId, dirInfo] : dirInfoMap_) {
      std::string dirpath = getDirPath(dirId);
      if (std::filesystem::exists(dirpath)) {
        if (dirInfo.isRecursive) {
          auto ukpDirDirStore = std::make_unique<DirDirStore>("dirdirstore");
          DirDirStore::Config ddConfig;
          ddConfig.dirPath = dirpath;
          ddConfig.maxFileCount = config_.maxFileCount;
          ddConfig.maxFileSize = config_.maxFileSize;
          ddConfig.maxDirCount = config_.maxDirCount;
          auto result = ukpDirDirStore->init(ddConfig);
          if (result.isOk()) {
            dirInfo.dirDirStore = std::move(ukpDirDirStore);
            log().debug << "Opened existing DirDirStore: " << dirpath
                        << " (blocks: " << dirInfo.dirDirStore->getBlockCount() << ")";
          } else {
            log().error << "Failed to open DirDirStore: " << dirpath << ": "
                        << result.error().message;
            return Error("Failed to open DirDirStore: " + dirpath + ": " +
                         result.error().message);
          }
        } else {
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
    }

    // Recalculate total block count from subdirectories
    totalBlockCount_ = 0;
    for (const auto &[dirId, dirInfo] : dirInfoMap_) {
      if (dirInfo.fileDirStore) {
        totalBlockCount_ += dirInfo.fileDirStore->getBlockCount();
      } else if (dirInfo.dirDirStore) {
        totalBlockCount_ += dirInfo.dirDirStore->getBlockCount();
      }
    }
  }

  log().info << "DirDirStore initialized with " << dirInfoMap_.size()
             << " subdirs and " << totalBlockCount_ << " total blocks"
             << (rootStore_ ? " (using root store)" : "");

  return {};
}

bool DirDirStore::canFit(uint64_t size) const {
  // Check if the data can fit in a single file at all
  if (size > config_.maxFileSize) {
    return false;
  }

  // If using root store, check it first
  if (rootStore_) {
    if (rootStore_->canFit(size)) {
      return true;
    }
    // Root store is full, but we can relocate it and continue
    return true;
  }

  // Check if current subdirectory can fit
  if (!dirInfoMap_.empty()) {
    auto it = dirInfoMap_.find(currentDirId_);
    if (it != dirInfoMap_.end()) {
      if (it->second.fileDirStore && it->second.fileDirStore->canFit(size)) {
        return true;
      } else if (it->second.dirDirStore && it->second.dirDirStore->canFit(size)) {
        return true;
      }
    }
    // Current store can't fit, check if we can create more
    if (dirInfoMap_.size() < config_.maxDirCount) {
      return true;
    }
    // At max dir count, can still go recursive
    return true;
  }

  return true;
}

uint64_t DirDirStore::getBlockCount() const { return totalBlockCount_; }

DirDirStore::Roe<std::string> DirDirStore::readBlock(uint64_t index) const {
  // If using root store and index is within its range
  if (rootStore_) {
    if (index < rootStore_->getBlockCount()) {
      return rootStore_->readBlock(index);
    }
    return Error("Block " + std::to_string(index) + " not found (root store has " +
                 std::to_string(rootStore_->getBlockCount()) + " blocks)");
  }

  // Find in subdirectories
  auto [dirId, indexWithinDir] = findBlockDir(index);
  if (dirId == 0 && indexWithinDir == 0 && index != 0) {
    return Error("Block " + std::to_string(index) + " not found");
  }

  auto it = dirInfoMap_.find(dirId);
  if (it == dirInfoMap_.end()) {
    return Error("Dir " + std::to_string(dirId) + " not found");
  }

  if (it->second.fileDirStore) {
    return it->second.fileDirStore->readBlock(indexWithinDir);
  } else if (it->second.dirDirStore) {
    return it->second.dirDirStore->readBlock(indexWithinDir);
  }

  return Error("Dir " + std::to_string(dirId) + " has no store");
}

DirDirStore::Roe<uint64_t> DirDirStore::appendBlock(const std::string &block) {
  // If using root store, try to write there first
  if (rootStore_) {
    if (rootStore_->canFit(block.size())) {
      auto result = rootStore_->appendBlock(block);
      if (!result.isOk()) {
        return Error("Failed to write to root store: " + result.error().message);
      }
      totalBlockCount_++;
      log().debug << "Wrote block " << totalBlockCount_ - 1 
                  << " to root store (size: " << block.size() << " bytes)";
      return totalBlockCount_ - 1;
    }

    // Root store is full, relocate it to a subdirectory
    auto relocateResult = relocateRootStore();
    if (!relocateResult.isOk()) {
      return Error("Failed to relocate root store: " + relocateResult.error().message);
    }
    
    // Now try again with subdirectory stores
    return appendBlock(block);
  }

  // Get active subdirectory store for writing
  DirStore *activeStore = getActiveDirStore(block.size());
  if (!activeStore) {
    log().error << "Failed to get active dir store";
    return Error("Failed to get active dir store");
  }

  // Write data to the store
  auto result = activeStore->appendBlock(block);
  if (!result.isOk()) {
    log().error << "Failed to write block to dir store";
    return Error("Failed to write block to dir store: " + result.error().message);
  }

  totalBlockCount_++;
  log().debug << "Wrote block " << totalBlockCount_ - 1 << " to dir "
              << currentDirId_ << " (size: " << block.size()
              << " bytes, total blocks: " << totalBlockCount_ << ")";

  saveIndex();
  return totalBlockCount_ - 1;
}

DirDirStore::Roe<void> DirDirStore::rewindTo(uint64_t index) {
  if (index > totalBlockCount_) {
    return Error("Cannot rewind to index " + std::to_string(index) +
                 " (max: " + std::to_string(totalBlockCount_) + ")");
  }

  // If using root store
  if (rootStore_) {
    auto rewindResult = rootStore_->rewindTo(index);
    if (!rewindResult.isOk()) {
      return Error("Failed to rewind root store: " + rewindResult.error().message);
    }
    totalBlockCount_ = rootStore_->getBlockCount();
    return {};
  }

  // Find the target subdirectory
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

  // Rewind the target dir
  if (dirId > 0) {
    auto it = dirInfoMap_.find(dirId);
    if (it != dirInfoMap_.end()) {
      if (it->second.fileDirStore) {
        auto rewindResult = it->second.fileDirStore->rewindTo(indexWithinDir);
        if (!rewindResult.isOk()) {
          return Error("Failed to rewind FileDirStore: " + rewindResult.error().message);
        }
      } else if (it->second.dirDirStore) {
        auto rewindResult = it->second.dirDirStore->rewindTo(indexWithinDir);
        if (!rewindResult.isOk()) {
          return Error("Failed to rewind DirDirStore: " + rewindResult.error().message);
        }
      }
    }
  }

  // Recalculate total block count
  totalBlockCount_ = 0;
  for (const auto &[did, dirInfo] : dirInfoMap_) {
    if (dirInfo.fileDirStore) {
      totalBlockCount_ += dirInfo.fileDirStore->getBlockCount();
    } else if (dirInfo.dirDirStore) {
      totalBlockCount_ += dirInfo.dirDirStore->getBlockCount();
    }
  }

  saveIndex();
  return {};
}

DirDirStore::Roe<void> DirDirStore::relocateRootStore() {
  if (!rootStore_) {
    return Error("No root store to relocate");
  }

  log().info << "Relocating root store to subdirectory";

  // Determine the subdirectory name (first available ID)
  currentDirId_ = 1;
  std::ostringstream oss;
  oss << std::setw(6) << std::setfill('0') << currentDirId_;
  std::string subdirName = oss.str();

  // Relocate the root store to become the first subdirectory
  auto relocateResult = rootStore_->relocateToSubdir(subdirName);
  if (!relocateResult.isOk()) {
    return Error("Failed to relocate root store: " + relocateResult.error().message);
  }

  // Add the relocated store to dirInfoMap_
  DirInfo dirInfo;
  dirInfo.fileDirStore = std::move(rootStore_);
  dirInfo.startBlockId = 0;
  dirInfo.isRecursive = false;
  dirInfoMap_[currentDirId_] = std::move(dirInfo);
  dirIdOrder_.push_back(currentDirId_);

  rootStore_.reset();

  log().info << "Root store relocated to subdirectory " << subdirName;
  saveIndex();

  return {};
}

// Private methods

DirStore *DirDirStore::getActiveDirStore(uint64_t dataSize) {
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
      it->second.isRecursive = true;
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
  dirInfo.isRecursive = true;
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
              << ", dirs: " << header.dirCount << ")";

  // Read dir entries
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
  log().debug << "Loaded " << dirInfoMap_.size() << " dir entries from index";

  return true;
}

bool DirDirStore::saveIndex() {
  // Only save index if we have subdirectories (not using root store)
  if (rootStore_ && dirInfoMap_.empty()) {
    // Root store manages its own index, we don't need a separate one
    return true;
  }

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

  // Write dir entries
  for (uint32_t dirId : dirIdOrder_) {
    auto it = dirInfoMap_.find(dirId);
    if (it == dirInfoMap_.end()) {
      continue;
    }

    DirIndexEntry entry(dirId, it->second.startBlockId,
                        it->second.dirDirStore != nullptr || it->second.isRecursive);
    std::string packed = utl::binaryPack(entry);
    indexFile.write(packed.data(), packed.size());
  }

  indexFile.close();
  log().debug << "Saved " << dirInfoMap_.size() << " dir entries to index";

  return true;
}

bool DirDirStore::writeIndexHeader(std::ostream &os) {
  IndexFileHeader header;
  header.dirCount = static_cast<uint32_t>(dirInfoMap_.size());
  OutputArchive ar(os);
  ar &header;

  if (!os.good()) {
    log().error << "Failed to write index file header";
    return false;
  }

  log().debug << "Wrote index file header (magic: 0x" << std::hex << header.magic
              << std::dec << ", version: " << header.version
              << ", dirs: " << header.dirCount << ")";

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
              << ", dirs: " << header.dirCount << ")";

  return true;
}

void DirDirStore::flush() {
  if (rootStore_) {
    // Root store handles its own flushing
    return;
  }
  
  if (!saveIndex()) {
    log().error << "Failed to save index during flush";
  }
}

DirDirStore::Roe<std::string> DirDirStore::relocateToSubdir(const std::string &subdirName) {
  log().info << "Relocating DirDirStore contents to subdirectory: " << subdirName;

  // If using root store, relocate it first
  if (rootStore_) {
    rootStore_.reset();
  }

  // Close all dir stores
  for (auto &[dirId, dirInfo] : dirInfoMap_) {
    dirInfo.fileDirStore.reset();
    dirInfo.dirDirStore.reset();
  }

  // Save current index before moving
  if (!dirInfoMap_.empty()) {
    if (!saveIndex()) {
      return Error("Failed to save index before relocation");
    }
  }

  std::string originalPath = config_.dirPath;
  std::filesystem::path originalDir(originalPath);
  std::filesystem::path parentDir = originalDir.parent_path();
  std::string dirName = originalDir.filename().string();
  std::string tempPath = (parentDir / (dirName + "_tmp_relocate")).string();
  std::string targetSubdir = originalPath + "/" + subdirName;

  std::error_code ec;

  // Step 1: Rename current dir to temp name (dir -> dir_tmp_relocate)
  std::filesystem::rename(originalPath, tempPath, ec);
  if (ec) {
    return Error("Failed to rename directory to temp: " + ec.message());
  }

  // Step 2: Create the original directory again
  if (!std::filesystem::create_directories(originalPath, ec)) {
    // Try to restore
    std::filesystem::rename(tempPath, originalPath, ec);
    return Error("Failed to recreate original directory: " + ec.message());
  }

  // Step 3: Rename temp to be a subdirectory of original (dir_tmp -> dir/subdirName)
  std::filesystem::rename(tempPath, targetSubdir, ec);
  if (ec) {
    // Try to restore
    std::filesystem::remove_all(originalPath, ec);
    std::filesystem::rename(tempPath, originalPath, ec);
    return Error("Failed to rename temp to subdirectory: " + ec.message());
  }

  // Update internal state to point to the new location
  config_.dirPath = targetSubdir;
  indexFilePath_ = targetSubdir + "/idx.dat";

  // Reopen the stores in the new location
  for (auto &[dirId, dirInfo] : dirInfoMap_) {
    std::string dirpath = getDirPath(dirId);
    if (std::filesystem::exists(dirpath)) {
      if (dirInfo.isRecursive) {
        auto ukpDirDirStore = std::make_unique<DirDirStore>("dirdirstore");
        DirDirStore::Config ddConfig;
        ddConfig.dirPath = dirpath;
        ddConfig.maxFileCount = config_.maxFileCount;
        ddConfig.maxFileSize = config_.maxFileSize;
        ddConfig.maxDirCount = config_.maxDirCount;
        auto result = ukpDirDirStore->init(ddConfig);
        if (result.isOk()) {
          dirInfo.dirDirStore = std::move(ukpDirDirStore);
          log().debug << "Reopened DirDirStore after relocation: " << dirpath;
        } else {
          log().error << "Failed to reopen DirDirStore after relocation: " << dirpath;
          return Error("Failed to reopen DirDirStore: " + result.error().message);
        }
      } else {
        auto ukpFileDirStore = std::make_unique<FileDirStore>("filedirstore");
        FileDirStore::Config fdConfig;
        fdConfig.dirPath = dirpath;
        fdConfig.maxFileCount = config_.maxFileCount;
        fdConfig.maxFileSize = config_.maxFileSize;
        auto result = ukpFileDirStore->init(fdConfig);
        if (result.isOk()) {
          dirInfo.fileDirStore = std::move(ukpFileDirStore);
          log().debug << "Reopened FileDirStore after relocation: " << dirpath;
        } else {
          log().error << "Failed to reopen FileDirStore after relocation: " << dirpath;
          return Error("Failed to reopen FileDirStore: " + result.error().message);
        }
      }
    }
  }

  log().info << "Successfully relocated DirDirStore to: " << targetSubdir;
  return targetSubdir;
}

} // namespace pp
