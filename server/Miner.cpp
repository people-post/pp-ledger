#include "Miner.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace pp {

Miner::Miner() {
  redirectLogger("Miner");
  chain_.redirectLogger(log().getFullName() + ".Chain");
}

bool Miner::isSlotLeader() const {
  return chain_.isStakeholderSlotLeader(config_.minerId, getCurrentSlot());
}

Miner::Roe<uint64_t> Miner::getSlotLeaderId() const {
  auto result = chain_.getSlotLeader(getCurrentSlot());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

uint64_t Miner::getStake() const {
  return chain_.getStakeholderStake(config_.minerId);
}

size_t Miner::getPendingTransactionCount() const { return pendingTxes_.size(); }

uint64_t Miner::getNextBlockId() const { return chain_.getNextBlockId(); }

uint64_t Miner::getCurrentSlot() const { return chain_.getCurrentSlot(); }

uint64_t Miner::getCurrentEpoch() const { return chain_.getCurrentEpoch(); }

std::vector<consensus::Stakeholder> Miner::getStakeholders() const {
  return chain_.getStakeholders();
}

Miner::Roe<Ledger::ChainNode> Miner::getBlock(uint64_t blockId) const {
  auto result = chain_.getBlock(blockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Miner::Roe<Client::UserAccount> Miner::getAccount(uint64_t accountId) const {
  auto result = chain_.getAccount(accountId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

std::string Miner::calculateHash(const Ledger::Block &block) const {
  return chain_.calculateHash(block);
}

Miner::Roe<void> Miner::init(const InitConfig &config) {
  config_.workDir = config.workDir;
  config_.minerId = config.minerId;
  config_.privateKey = config.privateKey;
  config_.checkpointId = config.checkpointId;

  log().info << "Initializing Miner";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Checkpoint ID: " << config_.checkpointId;

  // Create work directory if it doesn't exist
  if (!std::filesystem::exists(config.workDir)) {
    std::filesystem::create_directories(config.workDir);
  }

  log().info << "  Work directory: " << config.workDir;

  // Initialize ledger
  std::string ledgerDir = config.workDir + "/" + DIR_LEDGER;

  if (std::filesystem::exists(ledgerDir)) {
    auto roe = chain_.mountLedger(ledgerDir);
    if (!roe) {
      return Error(2, "Failed to mount ledger: " + roe.error().message);
    }
    if (getNextBlockId() < config.startingBlockId) {
      log().info << "Ledger data too old, removing existing work directory: "
                 << ledgerDir;
      std::error_code ec;
      std::filesystem::remove_all(ledgerDir, ec);
      if (ec) {
        return Error("Failed to remove existing work directory: " +
                     ec.message());
      }
    }
  }

  if (!std::filesystem::exists(ledgerDir)) {
    Ledger::InitConfig ledgerConfig;
    ledgerConfig.workDir = ledgerDir;
    ledgerConfig.startingBlockId = config.startingBlockId;
    auto ledgerResult = chain_.initLedger(ledgerConfig);
    if (!ledgerResult) {
      return Error(2, "Failed to initialize ledger: " +
                          ledgerResult.error().message);
    }
  }

  // Initialize consensus
  consensus::Ouroboros::Config consensusConfig;
  consensusConfig.timeOffset = config.timeOffset;
  chain_.initConsensus(consensusConfig);

  auto loadResult = chain_.loadFromLedger(config.startingBlockId);
  if (!loadResult) {
    return Error(2,
                 "Failed to load from ledger: " + loadResult.error().message);
  }

  log().info << "Miner initialized successfully";
  return {};
}

void Miner::refresh() {
  // Update miner state
  chain_.refreshStakeholders();
}

Miner::Roe<void> Miner::initSlotCache(uint64_t slot) {
  slotCache_ = {};
  slotCache_.slot = slot;
  slotCache_.isLeader = chain_.isStakeholderSlotLeader(config_.minerId, slot);
  if (slotCache_.isLeader) {
    // Leader initial evaluations per slot
    auto renewalsResult = chain_.collectRenewals(slot);
    if (!renewalsResult) {
      slotCache_ = {};
      return Error(12, renewalsResult.error().message);
    }
    slotCache_.txRenewals = renewalsResult.value();
    for (auto &signedTx : slotCache_.txRenewals) {
      auto message = utl::binaryPack(signedTx.obj);
      auto result = utl::ed25519Sign(config_.privateKey, message);
      if (!result) {
        slotCache_ = {};
        return Error(12, result.error().message);
      }
      signedTx.signatures.push_back(*result);
      auto addResult =
          chain_.addBufferTransaction(bufferBank_, signedTx, config_.minerId);
      if (!addResult) {
        slotCache_ = {};
        return Error(12, "Failed to add renewal transaction: " +
                             addResult.error().message);
      }
    }
  }
  return {};
}

Miner::Roe<bool> Miner::produceBlock(Ledger::ChainNode &block) {
  uint64_t slot = getCurrentSlot();
  // At most one block per slot
  if (lastProducedSlot_ == slot) {
    return false;
  }

  if (slotCache_.slot != slot) {
    // One time initialization of slot cache
    auto initResult = initSlotCache(slot);
    if (!initResult) {
      return Error(12, initResult.error().message);
    }
  }

  if (!slotCache_.isLeader) {
    // Only slot leader can produce block
    return false;
  }

  if (slotCache_.txRenewals.empty() && pendingTxes_.empty()) {
    // No transactions to produce block with
    return false;
  }

  // Only produce at end of current slot (within last second of slot)
  if (!chain_.isSlotBlockProductionTime(slot)) {
    return false;
  }

  log().info << "Producing block for slot " << slot;

  // Create the block
  auto createResult = createBlock(slot);
  if (!createResult) {
    return Error(7, "Failed to create block: " + createResult.error().message);
  }
  pendingTxes_.clear();

  block = createResult.value();
  return true;
}

void Miner::markBlockProduction(const Ledger::ChainNode &block) {
  lastProducedBlockId_ = block.block.index;
  lastProducedSlot_ = block.block.slot;
}

Miner::Roe<void>
Miner::addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  auto result =
      chain_.addBufferTransaction(bufferBank_, signedTx, config_.minerId);
  if (!result) {
    return Error(9, result.error().message);
  }

  pendingTxes_.push_back(signedTx);

  return {};
}

Miner::Roe<void> Miner::addBlock(const Ledger::ChainNode &block) {
  // Adding block is in strict mode if it is at or after the checkpoint id
  bool isStrictMode = block.block.index >= config_.checkpointId;
  // Call base class implementation which validates and adds to chain/ledger
  auto result = chain_.addBlock(block, isStrictMode);
  if (!result) {
    return Error(10, result.error().message);
  }

  return {};
}

// Private helper methods

Miner::Roe<Ledger::ChainNode> Miner::createBlock(uint64_t slot) {
  auto timestamp = chain_.getConsensusTimestamp();

  // Get previous block info
  auto latestBlockResult = chain_.readLastBlock();
  if (!latestBlockResult) {
    return Error(11, "Failed to read latest block: " +
                         latestBlockResult.error().message);
  }
  auto latestBlock = latestBlockResult.value();
  uint64_t blockIndex = latestBlock.block.index + 1;
  std::string previousHash = latestBlock.hash;

  // Create the block
  Ledger::ChainNode block;
  block.block.index = blockIndex;
  block.block.timestamp = timestamp;
  block.block.previousHash = previousHash;
  block.block.slot = slot;
  block.block.slotLeader = config_.minerId;

  // Populate signedTxes
  block.block.signedTxes = pendingTxes_;

  // Calculate hash
  block.hash = calculateHash(block.block);

  log().debug << "Created block " << blockIndex << " with "
              << pendingTxes_.size() << " transactions";

  return block;
}

} // namespace pp
