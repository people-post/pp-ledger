#include "Ledger.h"
#include "BinaryPack.hpp"
#include "Serialize.hpp"
#include "Utilities.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <sstream>

namespace pp {

std::string Ledger::AccountInfo::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Ledger::AccountInfo::ltsFromString(const std::string& str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

std::string Ledger::Block::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);

  // Serialize version and block fields
  uint16_t version = CURRENT_VERSION;
  ar & version & *this;

  return oss.str();
}

bool Ledger::Block::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);

  // Read version (uint16_t) - validate compatibility
  uint16_t version = 0;
  ar & version;
  if (ar.failed()) {
    return false;
  }

  // Validate version compatibility (can read current and future versions up
  // to a limit)
  if (version > CURRENT_VERSION) {
    return false; // Unsupported future version
  }

  // Deserialize block fields
  ar & *this;

  if (ar.failed()) {
    return false;
  }

  return true;
}

nlohmann::json Ledger::AccountInfo::toJson() const {
  nlohmann::json j;
  nlohmann::json balances;
  for (const auto& [tokenId, balance] : mBalances) {
    balances[std::to_string(tokenId)] = balance;
  }
  j["mBalances"] = balances;
  nlohmann::json keysArray = nlohmann::json::array();
  for (const auto& pk : publicKeys) {
    keysArray.push_back(utl::toJsonSafeString(pk));
  }
  j["publicKeys"] = keysArray;
  j["meta"] = utl::toJsonSafeString(meta);
  return j;
}

nlohmann::json Ledger::Transaction::toJson() const {
  nlohmann::json j;
  j["type"] = type;
  j["tokenId"] = tokenId;
  j["fromWalletId"] = fromWalletId;
  j["toWalletId"] = toWalletId;
  j["amount"] = amount;
  j["fee"] = fee;
  j["meta"] = utl::toJsonSafeString(meta);
  return j;
}

nlohmann::json Ledger::Block::toJson() const {
  nlohmann::json j;
  j["index"] = index;
  j["timestamp"] = timestamp;
  j["previousHash"] = utl::toJsonSafeString(previousHash);
  j["nonce"] = nonce;
  j["slot"] = slot;
  j["slotLeader"] = slotLeader;
  
  // Convert signed transactions to JSON array
  nlohmann::json txArray = nlohmann::json::array();
  for (const auto& signedTx : signedTxes) {
    nlohmann::json txJson;
    txJson["transaction"] = signedTx.obj.toJson();
    // Convert binary signatures to JSON-safe hex strings
    nlohmann::json sigArray = nlohmann::json::array();
    for (const auto& sig : signedTx.signatures) {
      sigArray.push_back(utl::toJsonSafeString(sig));
    }
    txJson["signatures"] = sigArray;
    txArray.push_back(txJson);
  }
  j["signedTransactions"] = txArray;
  
  return j;
}

nlohmann::json Ledger::ChainNode::toJson() const {
  nlohmann::json j;
  j["hash"] = utl::toJsonSafeString(hash);
  j["block"] = block.toJson();
  return j;
}

Ledger::Ledger() {
  redirectLogger("Ledger");
  store_.redirectLogger(log().getFullName() + ".Store");
}

uint64_t Ledger::getNextBlockId() const {
  uint64_t blockCount = store_.getBlockCount();
  // Next block ID = startingBlockId + blockCount
  // This handles both cases:
  // - No blocks: returns startingBlockId (the first block to be added)
  // - Has blocks: returns startingBlockId + blockCount (the next block after the last one)
  return meta_.startingBlockId + blockCount;
}

Ledger::Roe<void> Ledger::init(const InitConfig& config) {
  workDir_ = config.workDir;
  dataDir_ = workDir_ + "/data";
  indexFilePath_ = workDir_ + "/ledger_index.dat";
  
  // Verify work directory does NOT exist (fresh initialization)
  std::error_code ec;
  if (std::filesystem::exists(workDir_, ec)) {
    return Error("Work directory already exists: " + workDir_ + ". Use mount() to load existing ledger.");
  }

  // Create work directory
  if (!std::filesystem::create_directories(workDir_, ec)) {
    return Error("Failed to create work directory: " + workDir_);
  }

  log().info << "Ledger work directory created: " << workDir_;

  // Initialize DirDirStore with new directory
  DirDirStore::InitConfig storeConfig;
  storeConfig.dirPath = dataDir_;
  storeConfig.maxDirCount = 1000;    // Default values - can be made configurable
  storeConfig.maxFileCount = 1000;
  storeConfig.maxFileSize = 10 * 1024 * 1024; // 10 MB
  storeConfig.maxLevel = 2;

  auto initResult = store_.init(storeConfig);
  if (!initResult.isOk()) {
    return Error("Failed to initialize DirDirStore: " + initResult.error().message);
  }

  // Set starting block ID for fresh initialization
  meta_.startingBlockId = config.startingBlockId;

  // Save initial index with startingBlockId and checkpoints
  if (!saveIndex()) {
    return Error("Failed to save initial index");
  }

  log().info << "Ledger initialized at " << workDir_ 
            << " with startingBlockId=" << meta_.startingBlockId
            << ", nextBlockId=" << getNextBlockId();

  return {};
}

Ledger::Roe<void> Ledger::mount(const std::string& workDir) {
  workDir_ = workDir;
  dataDir_ = workDir_ + "/data";
  indexFilePath_ = workDir_ + "/ledger_index.dat";

  // Verify work directory exists (loading existing ledger)
  std::error_code ec;
  if (!std::filesystem::exists(workDir_, ec)) {
    return Error("Work directory does not exist: " + workDir_ + ". Use init() to create new ledger.");
  }

  log().info << "Mounting ledger at: " << workDir_;

  // Check if data directory exists
  bool dataExists = std::filesystem::exists(dataDir_, ec);
  if (!dataExists) {
    return Error("Ledger data directory not found: " + dataDir_);
  }

  // Load existing index to get current state
  if (!loadIndex()) {
    return Error("Failed to load ledger index from: " + indexFilePath_);
  }

  log().info << "Loaded existing ledger with startingBlockId=" << meta_.startingBlockId;

  // Mount existing DirDirStore (config values are loaded from index)
  DirDirStore::MountConfig storeConfig;
  storeConfig.dirPath = dataDir_;
  storeConfig.maxLevel = 2;

  auto mountResult = store_.mount(storeConfig);
  if (!mountResult.isOk()) {
    return Error("Failed to mount DirDirStore: " + mountResult.error().message);
  }

  log().info << "Ledger mounted successfully at " << workDir_ 
            << " with startingBlockId=" << meta_.startingBlockId
            << ", nextBlockId=" << getNextBlockId();

  return {};
}

Ledger::Roe<void> Ledger::addBlock(const Ledger::ChainNode& block) {
  // Serialize Block using Block::ltsToString()
  std::string blockData = block.block.ltsToString();
  
  // Create RawBlock with serialized block data and hash
  RawBlock rawBlock;
  rawBlock.data = blockData;
  rawBlock.hash = block.hash;
  
  // Append block to store
  auto appendResult = store_.appendBlock(utl::binaryPack(rawBlock));
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
  // Checkpoints must be less than the next block ID (i.e., <= last block ID)
  uint64_t nextBlockId = getNextBlockId();
  for (uint64_t checkpointId : blockIds) {
    if (checkpointId >= nextBlockId) {
      return Error("Checkpoint ID " + std::to_string(checkpointId) + 
                   " exceeds or equals next block ID " + std::to_string(nextBlockId));
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

Ledger::Roe<Ledger::ChainNode> Ledger::readBlock(uint64_t blockId) const {
  // Check if block ID is within valid range
  uint64_t nextBlockId = getNextBlockId();
  uint64_t blockCount = store_.getBlockCount();
  
  // If ledger is empty, any read should fail
  if (blockCount == 0) {
    return Error("Block ID " + std::to_string(blockId) + 
                 " exceeds last block ID (ledger is empty)");
  }
  
  // Block ID must be less than nextBlockId (i.e., <= last block ID)
  if (blockId >= nextBlockId) {
    return Error("Block ID " + std::to_string(blockId) + 
                 " exceeds or equals next block ID " + std::to_string(nextBlockId));
  }

  // Convert blockId to store index: index = blockId - startingBlockId
  if (blockId < meta_.startingBlockId) {
    return Error("Block ID " + std::to_string(blockId) + 
                 " is less than starting block ID " + std::to_string(meta_.startingBlockId));
  }
  uint64_t index = blockId - meta_.startingBlockId;

  // Read block data from store
  auto readResult = store_.readBlock(index);
  if (!readResult.isOk()) {
    return Error("Failed to read block " + std::to_string(blockId) + 
                 ": " + readResult.error().message);
  }

  // Deserialize RawBlock from binary string
  auto rawBlockResult = utl::binaryUnpack<Ledger::RawBlock>(readResult.value());
  if (!rawBlockResult.isOk()) {
    return Error("Failed to deserialize block " + std::to_string(blockId) + ": " + rawBlockResult.error().message);
  }
  Ledger::RawBlock rawBlock = rawBlockResult.value();

  // Deserialize Block from RawBlock's data string
  Ledger::Block block;
  if (!block.ltsFromString(rawBlock.data)) {
    return Error("Failed to deserialize block data " + std::to_string(blockId));
  }

  // Create ChainNode from deserialized Block and hash
  Ledger::ChainNode node;
  node.block = block;
  node.hash = rawBlock.hash;

  return node;
}

std::string Ledger::ChainNode::ltsToString() const {
  RawBlock rawBlock;
  rawBlock.data = block.ltsToString();
  rawBlock.hash = hash;
  return utl::binaryPack(rawBlock);
}

bool Ledger::ChainNode::ltsFromString(const std::string& str) {
  auto rawBlockResult = utl::binaryUnpack<RawBlock>(str);
  if (!rawBlockResult.isOk()) {
    return false;
  }
  RawBlock rawBlock = rawBlockResult.value();
  if (!block.ltsFromString(rawBlock.data)) {
    return false;
  }
  hash = rawBlock.hash;
  return true;
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

uint64_t Ledger::countSizeFromBlockId(uint64_t blockId) const {
  return store_.countSizeFromBlockId(blockId);
}

Ledger::Roe<Ledger::ChainNode> Ledger::readLastBlock() const {
  uint64_t nextBlockId = getNextBlockId();
  if (nextBlockId == 0) {
    return Error("No blocks in ledger");
  }
  return readBlock(nextBlockId - 1);
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
