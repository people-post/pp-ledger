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

DirDirStore::DirDirStore() {
  setLogger("DirDirStore");
}

DirDirStore::~DirDirStore() { flush(); }

std::string DirDirStore::getDirDirIndexFilePath(const std::string &dirPath) {
  return dirPath + "/" + DIRDIR_INDEX_FILENAME;
}

DirDirStore::Roe<void> DirDirStore::init(const InitConfig &config) {
  return initWithLevel(config, 0);
}

DirDirStore::Roe<void> DirDirStore::mount(const MountConfig &config) {
  return mountWithLevel(config, 0);
}

DirDirStore::Roe<void> DirDirStore::initWithLevel(const InitConfig &config, size_t level) {
  config_.dirPath = config.dirPath;
  config_.maxDirCount = config.maxDirCount;
  config_.maxFileCount = config.maxFileCount;
  config_.maxFileSize = config.maxFileSize;
  config_.maxLevel = config.maxLevel;
  currentDirId_ = 0;
  indexFilePath_ = getDirDirIndexFilePath(config_.dirPath);
  rootStore_.reset();
  dirInfoMap_.clear();
  dirIdOrder_.clear();
  totalBlockCount_ = 0;
  currentLevel_ = level;

  auto sizeResult = validateMinFileSize(config_.maxFileSize);
  if (!sizeResult.isOk()) {
    return sizeResult;
  }

  if (config_.maxFileCount == 0) {
    return Error("Max file count must be greater than 0");
  }

  if (config_.maxDirCount == 0) {
    return Error("Max dir count must be greater than 0");
  }

  // For init, verify index doesn't exist
  if (std::filesystem::exists(indexFilePath_)) {
    return Error("Index file already exists: " + indexFilePath_ + ". Use mount() to load existing directory.");
  }

  auto dirResult = ensureDirectory(config_.dirPath);
  if (!dirResult.isOk()) {
    return dirResult;
  }

  // Detect store mode from existing index
  auto modeResult = detectStoreMode();
  if (!modeResult.isOk()) {
    return Error(modeResult.error().message);
  }
  bool useRootStore = modeResult.value();

  // Initialize based on mode
  if (useRootStore) {
    auto initResult = initRootStoreMode(false);
    if (!initResult.isOk()) {
      return initResult;
    }
  } else {
    return Error("Cannot initialize new store with existing subdirectory structure");
  }

  log().info << "DirDirStore initialized at level " << currentLevel_ 
             << " with " << dirInfoMap_.size()
             << " subdirs and " << totalBlockCount_ << " total blocks"
             << (rootStore_ ? " (using root store)" : "");

  return {};
}

DirDirStore::Roe<void> DirDirStore::mountWithLevel(const MountConfig &config, size_t level) {
  config_.dirPath = config.dirPath;
  config_.maxLevel = config.maxLevel;
  currentDirId_ = 0;
  indexFilePath_ = getDirDirIndexFilePath(config_.dirPath);
  rootStore_.reset();
  dirInfoMap_.clear();
  dirIdOrder_.clear();
  totalBlockCount_ = 0;
  currentLevel_ = level;

  // For mount, verify directory and index exist
  if (!std::filesystem::exists(config_.dirPath)) {
    return Error("Directory does not exist: " + config_.dirPath);
  }
  if (!std::filesystem::exists(indexFilePath_)) {
    return Error("Index file does not exist: " + indexFilePath_);
  }

  auto dirResult = ensureDirectory(config_.dirPath);
  if (!dirResult.isOk()) {
    return dirResult;
  }

  // Detect store mode from existing index
  auto modeResult = detectStoreMode();
  if (!modeResult.isOk()) {
    return Error(modeResult.error().message);
  }
  bool useRootStore = modeResult.value();

  // Initialize based on mode
  if (useRootStore) {
    auto initResult = initRootStoreMode(true);
    if (!initResult.isOk()) {
      return initResult;
    }
  } else {
    auto openResult = openExistingSubdirectoryStores();
    if (!openResult.isOk()) {
      return openResult;
    }
    recalculateTotalBlockCount();
  }

  // Validate loaded config values
  auto sizeResult = validateMinFileSize(config_.maxFileSize);
  if (!sizeResult.isOk()) {
    return sizeResult;
  }

  if (config_.maxFileCount == 0) {
    return Error("Max file count must be greater than 0");
  }

  if (config_.maxDirCount == 0) {
    return Error("Max dir count must be greater than 0");
  }

  log().info << "DirDirStore mounted at level " << currentLevel_ 
             << " with " << dirInfoMap_.size()
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
    // After relocation, we become a subdirectory store with one FileDirStore
    // Can we fit more? Only if we can create more dirs or go recursive
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
    // Current store can't fit, check if we can create more subdirs
    if (dirInfoMap_.size() < config_.maxDirCount) {
      return true;
    }
    // At max dir count, check if we can go recursive (level control)
    if (!canCreateRecursive()) {
      // Cannot go recursive, store is full
      return false;
    }
    // Can create recursive DirDirStore children
    return true;
  }

  return true;
}

bool DirDirStore::canCreateRecursive() const {
  // Check if we're allowed to create recursive DirDirStore children
  // based on the maxLevel configuration
  return currentLevel_ < config_.maxLevel;
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
  std::string subdirName = formatId(currentDirId_);

  // Relocate the root store to become the first subdirectory
  // Preserve the DirDirStore index file in the parent directory
  std::vector<std::string> excludeFiles = {DIRDIR_INDEX_FILENAME};
  auto relocateResult = rootStore_->relocateToSubdir(subdirName, excludeFiles);
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
    // Check if we're allowed to create recursive DirDirStore children
    if (!canCreateRecursive()) {
      log().error << "Reached max dir count " << config_.maxDirCount 
                  << " at level " << currentLevel_ 
                  << " (max level: " << config_.maxLevel << "), cannot create recursive stores";
      return nullptr;
    }

    // Breadth-first: First try to find an existing DirDirStore child that can fit
    for (auto &[dirId, dirInfo] : dirInfoMap_) {
      if (dirInfo.dirDirStore && dirInfo.dirDirStore->canFit(dataSize)) {
        currentDirId_ = dirId;
        return dirInfo.dirDirStore.get();
      }
    }

    // No existing DirDirStore can fit, need to create a new one
    // Check if we should transition a FileDirStore or create a new DirDirStore
    if (it != dirInfoMap_.end() && it->second.fileDirStore) {
      // Transition current FileDirStore to DirDirStore
      std::string dirpath = getDirPath(currentDirId_);
      auto ukpDirDirStore = std::make_unique<DirDirStore>();
      ukpDirDirStore->setLogger("dirdirstore");
      DirDirStore::InitConfig ddConfig;
      ddConfig.dirPath = dirpath;
      ddConfig.maxFileCount = config_.maxFileCount;
      ddConfig.maxFileSize = config_.maxFileSize;
      ddConfig.maxDirCount = config_.maxDirCount;
      // Breadth-first: Check if all OTHER children (excluding current) are DirDirStore
      // If yes, allow deeper recursion. If no, only allow FileDirStore children.
      bool allOtherChildrenAreRecursive = true;
      for (const auto &[did, dinfo] : dirInfoMap_) {
        if (did != currentDirId_ && dinfo.fileDirStore) {
          allOtherChildrenAreRecursive = false;
          break;
        }
      }
      if (allOtherChildrenAreRecursive && canCreateRecursive()) {
        // All other children are DirDirStore, allow deeper recursion
        ddConfig.maxLevel = config_.maxLevel;
      } else {
        // Still have FileDirStore children, only allow FileDirStore in new DirDirStore
        ddConfig.maxLevel = currentLevel_ + 1;  // Can only create FileDirStore children
      }
      // Child level is currentLevel_ + 1
      auto result = ukpDirDirStore->initWithLevel(ddConfig, currentLevel_ + 1);
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
  auto ukpFileDirStore = std::make_unique<FileDirStore>();
  ukpFileDirStore->setLogger("filedirstore");

  FileDirStore::InitConfig fdConfig;
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
  auto ukpDirDirStore = std::make_unique<DirDirStore>();
  ukpDirDirStore->setLogger("dirdirstore");

  DirDirStore::InitConfig ddConfig;
  ddConfig.dirPath = dirpath;
  ddConfig.maxFileCount = config_.maxFileCount;
  ddConfig.maxFileSize = config_.maxFileSize;
  ddConfig.maxDirCount = config_.maxDirCount;
  // Breadth-first: Check if all OTHER children (excluding the one we're creating) are DirDirStore
  // If yes, allow deeper recursion. If no, only allow FileDirStore children.
  bool allOtherChildrenAreRecursive = true;
  for (const auto &[did, dinfo] : dirInfoMap_) {
    if (did != dirId && dinfo.fileDirStore) {
      allOtherChildrenAreRecursive = false;
      break;
    }
  }
  if (allOtherChildrenAreRecursive && canCreateRecursive()) {
    // All other children are DirDirStore, allow deeper recursion
    ddConfig.maxLevel = config_.maxLevel;
  } else {
    // Still have FileDirStore children, only allow FileDirStore in new DirDirStore
    ddConfig.maxLevel = currentLevel_ + 1;  // Can only create FileDirStore children
  }
  // Child level is currentLevel_ + 1
  auto result = ukpDirDirStore->initWithLevel(ddConfig, currentLevel_ + 1);
  if (!result.isOk()) {
    log().error << "Failed to create DirDirStore: " << dirpath;
    return nullptr;
  }

  log().info << "Created new recursive DirDirStore at level " << (currentLevel_ + 1) 
             << ": " << dirpath << " (startBlockId: " << startBlockId << ")";

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
  return config_.dirPath + "/" + formatId(dirId);
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

  // Load config values from header
  config_.maxDirCount = header.maxDirCount;
  config_.maxFileCount = header.maxFileCount;
  config_.maxFileSize = header.maxFileSize;
  log().debug << "Loaded config from index: maxDirCount=" << config_.maxDirCount
              << ", maxFileCount=" << config_.maxFileCount
              << ", maxFileSize=" << config_.maxFileSize;

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

    DirIndexEntry entry;
    entry.dirId = dirId;
    entry.startBlockId = it->second.startBlockId;
    entry.isRecursive = it->second.dirDirStore != nullptr || it->second.isRecursive;
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
  header.maxDirCount = config_.maxDirCount;
  header.maxFileCount = config_.maxFileCount;
  header.maxFileSize = config_.maxFileSize;
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

DirDirStore::Roe<std::string> DirDirStore::relocateToSubdir(const std::string &subdirName,
                                                             const std::vector<std::string> &excludeFiles) {
  log().info << "Relocating DirDirStore contents to subdirectory: " << subdirName;

  if (rootStore_) {
    rootStore_.reset();
  }

  for (auto &[dirId, dirInfo] : dirInfoMap_) {
    dirInfo.fileDirStore.reset();
    dirInfo.dirDirStore.reset();
  }

  if (!dirInfoMap_.empty() && !saveIndex()) {
    return Error("Failed to save index before relocation");
  }

  std::string originalPath = config_.dirPath;
  auto relocateResult = performDirectoryRelocation(originalPath, subdirName, excludeFiles);
  if (!relocateResult.isOk()) {
    return relocateResult;
  }
  std::string targetSubdir = relocateResult.value();

  config_.dirPath = targetSubdir;
  indexFilePath_ = getDirDirIndexFilePath(targetSubdir);

  auto reopenResult = reopenSubdirectoryStores();
  if (!reopenResult.isOk()) {
    return Error(reopenResult.error().message);
  }

  log().info << "Successfully relocated DirDirStore to: " << targetSubdir;
  return targetSubdir;
}

// Helper methods

DirDirStore::Roe<bool> DirDirStore::detectStoreMode() {
  if (!std::filesystem::exists(indexFilePath_)) {
    log().info << "No existing index file, starting fresh";
    return true; // Use root store mode
  }

  std::ifstream checkFile(indexFilePath_, std::ios::binary);
  if (!checkFile.is_open()) {
    return true; // Default to root store mode
  }

  uint32_t magic = 0;
  InputArchive ar(checkFile);
  ar &magic;
  checkFile.close();

  if (ar.failed()) {
    log().error << "Failed to read magic from index file";
    return Error("Failed to read magic from index file");
  }

  if (magic == MAGIC_DIR_DIR) {
    if (!loadIndex()) {
      log().error << "Failed to load index file";
      return Error("Failed to load index file");
    }
    log().info << "Loaded index with " << dirInfoMap_.size() << " dirs";
    updateCurrentDirId();
    // If no directories in the map, we're in root store mode
    return dirInfoMap_.empty();
  } else if (magic == MAGIC_FILE_DIR) {
    log().info << "Found FileDirStore index, using root store mode";
    return true;
  } else {
    log().error << "Unknown magic number in index file: 0x" << std::hex << magic << std::dec;
    return Error("Unknown magic number in index file");
  }
}

DirDirStore::Roe<void> DirDirStore::initRootStoreMode(bool isMount) {
  rootStore_ = std::make_unique<FileDirStore>();
  rootStore_->setLogger("root-filedirstore");
  FileDirStore::InitConfig fdConfig;
  fdConfig.dirPath = config_.dirPath;
  fdConfig.maxFileCount = config_.maxFileCount;
  fdConfig.maxFileSize = config_.maxFileSize;

  if (isMount) {
    // Mount existing FileDirStore
    auto result = rootStore_->mount(config_.dirPath);
    if (!result.isOk()) {
      log().error << "Failed to mount root FileDirStore: " << result.error().message;
      return Error("Failed to mount root FileDirStore: " + result.error().message);
    }
    totalBlockCount_ = rootStore_->getBlockCount();
    log().info << "Mounted root FileDirStore with " << totalBlockCount_ << " blocks";
  } else {
    // Initialize new FileDirStore
    auto result = rootStore_->init(fdConfig);
    if (!result.isOk()) {
      log().error << "Failed to initialize root FileDirStore: " << result.error().message;
      return Error("Failed to initialize root FileDirStore: " + result.error().message);
    }
    totalBlockCount_ = rootStore_->getBlockCount();
    log().info << "Initialized root FileDirStore with " << totalBlockCount_ << " blocks";
    
    // Save index file with config values even in root store mode
    if (!saveIndex()) {
      log().error << "Failed to save initial index file";
      return Error("Failed to save initial index file");
    }
  }
  
  return {};
}

DirDirStore::Roe<void> DirDirStore::openExistingSubdirectoryStores() {
  for (auto &[dirId, dirInfo] : dirInfoMap_) {
    std::string dirpath = getDirPath(dirId);
    if (!std::filesystem::exists(dirpath)) {
      continue;
    }

    auto result = openDirStore(dirInfo, dirId, dirpath);
    if (!result.isOk()) {
      return result;
    }
  }
  return {};
}

DirDirStore::Roe<void> DirDirStore::reopenSubdirectoryStores() {
  for (auto &[dirId, dirInfo] : dirInfoMap_) {
    std::string dirpath = getDirPath(dirId);
    if (!std::filesystem::exists(dirpath)) {
      continue;
    }

    auto result = openDirStore(dirInfo, dirId, dirpath);
    if (!result.isOk()) {
      log().error << "Failed to reopen store after relocation: " << dirpath;
      return result;
    }
    log().debug << "Reopened store after relocation: " << dirpath;
  }
  return {};
}

DirDirStore::Roe<void> DirDirStore::openDirStore(DirInfo &dirInfo, uint32_t dirId, const std::string &dirpath) {
  if (dirInfo.isRecursive) {
    auto ukpDirDirStore = std::make_unique<DirDirStore>();
    ukpDirDirStore->setLogger("dirdirstore");
    DirDirStore::MountConfig ddConfig;
    ddConfig.dirPath = dirpath;
    // Breadth-first: Check if all OTHER children (excluding the one we're opening) are DirDirStore
    // If yes, allow deeper recursion. If no, only allow FileDirStore children.
    bool allOtherChildrenAreRecursive = true;
    for (const auto &[did, dinfo] : dirInfoMap_) {
      if (did != dirId && dinfo.fileDirStore) {
        allOtherChildrenAreRecursive = false;
        break;
      }
    }
    if (allOtherChildrenAreRecursive && canCreateRecursive()) {
      // All other children are DirDirStore, allow deeper recursion
      ddConfig.maxLevel = config_.maxLevel;
    } else {
      // Still have FileDirStore children, only allow FileDirStore in new DirDirStore
      ddConfig.maxLevel = currentLevel_ + 1;  // Can only create FileDirStore children
    }

    // Child level is currentLevel_ + 1
    // Note: maxDirCount, maxFileCount, maxFileSize will be loaded from the child's index file
    auto result = ukpDirDirStore->mountWithLevel(ddConfig, currentLevel_ + 1);
    if (!result.isOk()) {
      log().error << "Failed to open DirDirStore: " << dirpath << ": " << result.error().message;
      return Error("Failed to open DirDirStore: " + dirpath + ": " + result.error().message);
    }

    dirInfo.dirDirStore = std::move(ukpDirDirStore);
    log().debug << "Opened DirDirStore at level " << (currentLevel_ + 1) << ": " << dirpath
                << " (blocks: " << dirInfo.dirDirStore->getBlockCount() << ")";
  } else {
    auto ukpFileDirStore = std::make_unique<FileDirStore>();
    ukpFileDirStore->setLogger("filedirstore");

    auto result = ukpFileDirStore->mount(dirpath);
    if (!result.isOk()) {
      log().error << "Failed to open FileDirStore: " << dirpath << ": " << result.error().message;
      return Error("Failed to open FileDirStore: " + dirpath + ": " + result.error().message);
    }

    dirInfo.fileDirStore = std::move(ukpFileDirStore);
    log().debug << "Opened FileDirStore: " << dirpath
                << " (blocks: " << dirInfo.fileDirStore->getBlockCount() << ")";
  }
  return {};
}

void DirDirStore::recalculateTotalBlockCount() {
  totalBlockCount_ = 0;
  for (const auto &[dirId, dirInfo] : dirInfoMap_) {
    if (dirInfo.fileDirStore) {
      totalBlockCount_ += dirInfo.fileDirStore->getBlockCount();
    } else if (dirInfo.dirDirStore) {
      totalBlockCount_ += dirInfo.dirDirStore->getBlockCount();
    }
  }
}

void DirDirStore::updateCurrentDirId() {
  for (const auto &[dirId, _] : dirInfoMap_) {
    if (dirId > currentDirId_) {
      currentDirId_ = dirId;
    }
  }
}

} // namespace pp
