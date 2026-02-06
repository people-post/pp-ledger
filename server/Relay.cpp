#include "Relay.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include "../lib/BinaryPack.hpp"
#include "../ledger/Ledger.h"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

Relay::Relay() {}

uint64_t Relay::getLastCheckpointId() const {
  return 0;
}

uint64_t Relay::getCurrentCheckpointId() const {
  return 0;
}

Relay::Roe<void> Relay::init(const InitConfig& config) {
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
    auto roe = getLedger().mount(ledgerDir);
    if (!roe) {
      return Error(2, "Failed to mount ledger: " + roe.error().message);
    }
    if (getLedger().getNextBlockId() < config.startingBlockId) {
      log().info << "Ledger data too old, removing existing work directory: " << ledgerDir;
      std::error_code ec;
      std::filesystem::remove_all(ledgerDir, ec);
      if (ec) {
        return Error("Failed to remove existing work directory: " + ec.message());
      }
    }
  }

  if (!std::filesystem::exists(ledgerDir)) {
    Ledger::InitConfig ledgerConfig;
    ledgerConfig.workDir = ledgerDir;
    ledgerConfig.startingBlockId = config.startingBlockId;
    auto ledgerResult = getLedger().init(ledgerConfig);
    if (!ledgerResult) {
      return Error(2, "Failed to initialize ledger: " + ledgerResult.error().message);
    }
  }

  // Initialize consensus (timeOffset only; full config from genesis block when loading)
  consensus::Ouroboros::Config cc;
  cc.timeOffset = config.timeOffset;
  getConsensus().init(cc);

  auto loadResult = loadFromLedger(config.startingBlockId);
  if (!loadResult) {
    return Error(2, "Failed to load from ledger: " + loadResult.error().message);
  }

  log().info << "Relay initialized successfully";
  log().info << "  Starting block ID: " << config.startingBlockId;
  log().info << "  Next block ID: " << getNextBlockId();

  return {};
}

Relay::Roe<void> Relay::addBlock(const Ledger::ChainNode& block) {
  // Relay starts at block 0, use strict validation
  auto result = addBlockBase(block, true);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return {};
}

} // namespace pp
