#include "Relay.h"
#include "../ledger/Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace pp {

std::ostream &operator<<(std::ostream &os, const Relay::InitConfig &config) {
  os << "InitConfig{workDir=\"" << config.workDir << "\", "
     << "timeOffset=" << config.timeOffset << ", "
     << "startingBlockId=" << config.startingBlockId << "}";
  return os;
}

Relay::Relay() {
  redirectLogger("Relay");
  chain_.redirectLogger(log().getFullName() + ".Chain");
}

uint64_t Relay::getLastCheckpointId() const {
  return chain_.getLastCheckpointId();
}

uint64_t Relay::getCurrentCheckpointId() const {
  return chain_.getCurrentCheckpointId();
}

uint64_t Relay::getNextBlockId() const { return chain_.getNextBlockId(); }

uint64_t Relay::getCurrentSlot() const { return chain_.getCurrentSlot(); }

uint64_t Relay::getCurrentEpoch() const { return chain_.getCurrentEpoch(); }

std::vector<consensus::Stakeholder> Relay::getStakeholders() const {
  return chain_.getStakeholders();
}

Relay::Roe<Ledger::ChainNode> Relay::getBlock(uint64_t blockId) const {
  auto result = chain_.getBlock(blockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Relay::Roe<Client::UserAccount> Relay::getAccount(uint64_t accountId) const {
  auto result = chain_.getAccount(accountId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

std::string Relay::calculateHash(const Ledger::Block &block) const {
  return chain_.calculateHash(block);
}

Relay::Roe<void> Relay::init(const InitConfig &config) {
  log().info << "Initializing Relay";
  log().debug << "Init config: " << config;

  config_.workDir = config.workDir;

  // Create work directory if it doesn't exist
  if (!std::filesystem::exists(config.workDir)) {
    std::filesystem::create_directories(config.workDir);
  }
  log().info << "  Work directory: " << config.workDir;

  // Initialize ledger with startingBlockId (0 for relay)
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

  // Initialize consensus (timeOffset only; full config from genesis block when
  // loading)
  consensus::Ouroboros::Config consensusConfig;
  consensusConfig.timeOffset = config.timeOffset;
  chain_.initConsensus(consensusConfig);

  auto loadResult = chain_.loadFromLedger(config.startingBlockId);
  if (!loadResult) {
    return Error(2,
                 "Failed to load from ledger: " + loadResult.error().message);
  }

  log().info << "Relay initialized successfully";
  log().info << "  Starting block ID: " << config.startingBlockId;
  log().info << "  Next block ID: " << getNextBlockId();

  return {};
}

void Relay::refresh() {
  // Update relay state
  chain_.refreshStakeholders();
}

Relay::Roe<void> Relay::addBlock(const Ledger::ChainNode &block) {
  // Relay starts at block 0, use strict validation
  auto result = chain_.addBlock(block, true);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return {};
}

} // namespace pp
