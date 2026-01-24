#include "Ledger.h"
#include "BinaryPack.hpp"
#include "Serialize.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace pp {

uint64_t Ledger::getCurrentBlockId() const {
  // Return 0 if no data found
  uint64_t blockCount = store_.getBlockCount();
  if (blockCount == 0) {
    return 0;
  }
  // Block IDs are 0-based, so currentBlockId = blockCount - 1
  return blockCount - 1;
}

Ledger::Roe<void> Ledger::init(const Config& config) {
  workDir_ = config.workDir;
  dataDir_ = workDir_ + "/data";
  indexFilePath_ = workDir_ + "/ledger_index.dat";
  meta_.startingBlockId = config.startingBlockId;
  
  // Ensure work directory exists
  std::error_code ec;
  if (!std::filesystem::exists(workDir_, ec)) {
    if (!std::filesystem::create_directories(workDir_, ec)) {
      return Error("Failed to create work directory: " + workDir_);
    }
  }

  // Check if data directory exists
  bool dataExists = std::filesystem::exists(dataDir_, ec);
  uint64_t existingStartingBlockId = 0;

  if (dataExists) {
    // Load existing index to get current state
    if (loadIndex()) {
      // After loading, meta_ is populated from the index
      existingStartingBlockId = meta_.startingBlockId;
      log().info << "Loaded existing ledger with startingBlockId=" << existingStartingBlockId;
    } else {
      log().warning << "Data directory exists but no valid index found";
    }
  }

  // Initialize DirDirStore first to get the current block count
  DirDirStore::Config storeConfig;
  storeConfig.dirPath = dataDir_;
  storeConfig.maxDirCount = 1000;    // Default values - can be made configurable
  storeConfig.maxFileCount = 1000;
  storeConfig.maxFileSize = 10 * 1024 * 1024; // 10 MB
  storeConfig.maxLevel = 2;

  auto initResult = store_.init(storeConfig);
  if (!initResult.isOk()) {
    return Error("Failed to initialize DirDirStore: " + initResult.error().message);
  }

  // Now check if we need to cleanup based on currentBlockId
  if (dataExists) {
    uint64_t currentBlockId = store_.getBlockCount();
    
    // If startingBlockId is newer than the currentBlockId, cleanup and start fresh
    if (config.startingBlockId > currentBlockId) {
      log().info << "Starting block ID (" << config.startingBlockId 
                << ") is newer than current block ID (" << currentBlockId 
                << "). Cleaning up and starting fresh.";
      
      auto cleanupResult = cleanupData();
      if (!cleanupResult.isOk()) {
        return cleanupResult;
      }
      
      meta_.checkpointIds.clear();
      
      // Re-initialize the store after cleanup
      auto reinitResult = store_.init(storeConfig);
      if (!reinitResult.isOk()) {
        return Error("Failed to re-initialize DirDirStore: " + reinitResult.error().message);
      }
    } else {
      log().info << "Loaded existing ledger with currentBlockId=" << currentBlockId;
    }
  }

  // Set the startingBlockId for this session
  meta_.startingBlockId = config.startingBlockId;

  // Save initial index with startingBlockId and checkpoints
  if (!saveIndex()) {
    return Error("Failed to save initial index");
  }

  log().info << "Ledger initialized at " << workDir_ 
            << " with startingBlockId=" << meta_.startingBlockId
            << ", currentBlockId=" << getCurrentBlockId();

  return {};
}

Ledger::Roe<void> Ledger::addBlock(const Block& block) {
  // Append block to store
  auto appendResult = store_.appendBlock(block.ltsToString());
  if (!appendResult.isOk()) {
    return Error("Failed to append block: " + appendResult.error().message);
  }

  // Save index after adding block
  if (!saveIndex()) {
    return Error("Failed to save index after adding block");
  }

  return {};
}

Ledger::Roe<void> Ledger::updateCheckpoints(const std::vector<uint64_t>& blockIds) {
  // Verify input is sorted in ascending order
  if (!std::is_sorted(blockIds.begin(), blockIds.end())) {
    return Error("Checkpoint IDs must be sorted in ascending order");
  }

  // Check for duplicates
  for (size_t i = 1; i < blockIds.size(); ++i) {
    if (blockIds[i] == blockIds[i - 1]) {
      return Error("Checkpoint IDs must not contain duplicates");
    }
  }

  // Verify existing checkpoint IDs match the input sequence if there's overlap
  size_t minSize = std::min(meta_.checkpointIds.size(), blockIds.size());
  for (size_t i = 0; i < minSize; ++i) {
    if (meta_.checkpointIds[i] != blockIds[i]) {
      return Error("Checkpoint ID mismatch at index " + std::to_string(i) + 
                   ": existing=" + std::to_string(meta_.checkpointIds[i]) + 
                   ", new=" + std::to_string(blockIds[i]));
    }
  }

  // Verify all checkpoint IDs are within valid range
  uint64_t currentBlockId = getCurrentBlockId();
  for (uint64_t checkpointId : blockIds) {
    if (checkpointId > currentBlockId) {
      return Error("Checkpoint ID " + std::to_string(checkpointId) + 
                   " exceeds current block ID " + std::to_string(currentBlockId));
    }
  }

  // Update checkpoint IDs
  meta_.checkpointIds = blockIds;

  // Save index with updated checkpoints
  if (!saveIndex()) {
    return Error("Failed to save index after updating checkpoints");
  }

  log().info << "Updated checkpoints: count=" << meta_.checkpointIds.size();

  return {};
}

Ledger::Roe<Block> Ledger::readBlock(uint64_t blockId) const {
  // Check if block ID is within valid range
  uint64_t currentBlockId = getCurrentBlockId();
  uint64_t blockCount = store_.getBlockCount();
  
  // If ledger is empty, any read should fail
  if (blockCount == 0) {
    return Error("Block ID " + std::to_string(blockId) + 
                 " exceeds current block ID (ledger is empty)");
  }
  
  if (blockId > currentBlockId) {
    return Error("Block ID " + std::to_string(blockId) + 
                 " exceeds current block ID " + std::to_string(currentBlockId));
  }

  // Block IDs are 0-based, so blockId is the index
  uint64_t index = blockId;

  // Read block data from store
  auto readResult = store_.readBlock(index);
  if (!readResult.isOk()) {
    return Error("Failed to read block " + std::to_string(blockId) + 
                 ": " + readResult.error().message);
  }

  // Deserialize block from binary string
  Block block;
  if (!block.ltsFromString(readResult.value())) {
    return Error("Failed to deserialize block " + std::to_string(blockId));
  }

  return block;
}

bool Ledger::loadIndex() {
  std::ifstream file(indexFilePath_, std::ios::binary);
  if (!file) {
    return false;
  }

  try {
    InputArchive ar(file);
    
    // Read header
    IndexFileHeader header;
    ar & header;

    if (ar.failed()) {
      log().error << "Failed to deserialize index header";
      return false;
    }

    // Verify magic and version
    if (header.magic != IndexFileHeader::MAGIC) {
      log().error << "Invalid index file magic: " << std::hex << header.magic;
      return false;
    }

    if (header.version != IndexFileHeader::CURRENT_VERSION) {
      log().error << "Unsupported index file version: " << header.version;
      return false;
    }

    // Read metadata body
    ar & meta_;

    if (ar.failed()) {
      log().error << "Failed to deserialize metadata";
      return false;
    }

    log().info << "Loaded index: startingBlockId=" << meta_.startingBlockId 
              << ", checkpoints=" << meta_.checkpointIds.size();

    return true;
  } catch (const std::exception& e) {
    log().error << "Exception loading index: " << e.what();
    return false;
  }
}

bool Ledger::saveIndex() {
  try {
    // Write to temporary file first
    std::string tempPath = indexFilePath_ + ".tmp";
    std::ofstream file(tempPath, std::ios::binary);
    if (!file) {
      log().error << "Failed to open index file for writing: " << tempPath;
      return false;
    }

    OutputArchive ar(file);
    
    // Write header
    IndexFileHeader header;
    ar & header;

    // Write metadata body
    ar & meta_;

    if (!file.good()) {
      log().error << "Failed to write index file";
      return false;
    }

    file.close();

    // Atomic rename
    std::error_code ec;
    std::filesystem::rename(tempPath, indexFilePath_, ec);
    if (ec) {
      log().error << "Failed to rename index file: " << ec.message();
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    log().error << "Exception saving index: " << e.what();
    return false;
  }
}

Ledger::Roe<void> Ledger::cleanupData() {
  std::error_code ec;
  
  // Remove data directory
  if (std::filesystem::exists(dataDir_, ec)) {
    std::filesystem::remove_all(dataDir_, ec);
    if (ec) {
      return Error("Failed to remove data directory: " + ec.message());
    }
  }

  // Remove index file
  if (std::filesystem::exists(indexFilePath_, ec)) {
    std::filesystem::remove(indexFilePath_, ec);
    if (ec) {
      return Error("Failed to remove index file: " + ec.message());
    }
  }

  log().info << "Cleaned up ledger data at " << workDir_;
  
  return {};
}

} // namespace pp
