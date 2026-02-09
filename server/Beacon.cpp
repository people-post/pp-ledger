#include "Beacon.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include "../lib/BinaryPack.hpp"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

// Ostream operators for easy logging
std::ostream& operator<<(std::ostream& os, const Beacon::InitConfig& config) {
  os << "InitConfig{workDir=\"" << config.workDir << "\", "
     << "chain=" << config.chain << "}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const Beacon::MountConfig& config) {
  os << "MountConfig{workDir=\"" << config.workDir << "\"}";
  return os;
}

Beacon::Beacon() {}

nlohmann::json Beacon::InitKeyConfig::toJson() const {
  nlohmann::json j;
  for (const auto& kp : genesis) {
    j["genesis"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)}, {"privateKey", utl::hexEncode(kp.privateKey)}});
  }
  for (const auto& kp : fee) {
    j["fee"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)}, {"privateKey", utl::hexEncode(kp.privateKey)}});
  }
  for (const auto& kp : reserve) {
    j["reserve"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)}, {"privateKey", utl::hexEncode(kp.privateKey)}});
  }
  return j;
}

Beacon::Roe<void> Beacon::init(const InitConfig& config) {
  log().info << "Initializing Beacon";
  log().debug << "Init config: " << config;

  // Verify work directory does NOT exist (fresh initialization)
  if (std::filesystem::exists(config.workDir)) {
    return Error("Work directory already exists: " + config.workDir + ". Use mount() to load existing beacon.");
  }

  // Create work directory
  std::filesystem::create_directories(config.workDir);
  log().info << "  Work directory created: " << config.workDir;

  // Initialize consensus
  consensus::Ouroboros::Config consensusConfig;
  consensusConfig.genesisTime = utl::getCurrentTime();
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = config.chain.slotDuration;
  consensusConfig.slotsPerEpoch = config.chain.slotsPerEpoch;
  initConsensus(consensusConfig);
  
  // Initialize ledger
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = config.workDir + "/ledger";
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = initLedger(ledgerConfig);
  if (!ledgerResult) {
    return Error(2, "Failed to initialize ledger: " + ledgerResult.error().message);
  }

  config_.workDir = config.workDir;
  auto chainConfig = config.chain;
  chainConfig.genesisTime = consensusConfig.genesisTime;

  // Create and add genesis block
  auto genesisBlockResult = createGenesisBlock(chainConfig, config.key);
  if (!genesisBlockResult) {
    return Error(2, "Failed to create genesis block: " + genesisBlockResult.error().message);
  }
  auto genesisBlock = genesisBlockResult.value();
  auto addBlockResult = addBlock(genesisBlock);
  if (!addBlockResult) {
    return Error(2, "Failed to add genesis block: " + addBlockResult.error().message);
  }
  
  log().info << "Genesis block created with checkpoint transaction (version " 
             << SystemCheckpoint::VERSION << ")";

  log().info << "Beacon initialized successfully";
  log().info << "  Genesis time: " << consensusConfig.genesisTime;
  log().info << "  Time offset: " << consensusConfig.timeOffset;
  log().info << "  Slot duration: " << consensusConfig.slotDuration;
  log().info << "  Slots per epoch: " << consensusConfig.slotsPerEpoch;
  log().info << "  Max pending transactions: " << chainConfig.maxPendingTransactions;
  log().info << "  Max transactions per block: " << chainConfig.maxTransactionsPerBlock;
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

Beacon::Roe<void> Beacon::mount(const MountConfig& config) {
  log().info << "Mounting Beacon at: " << config.workDir;
  log().debug << "Mount config: " << config;

  // Verify work directory exists (loading existing state)
  if (!std::filesystem::exists(config.workDir)) {
    return Error(3, "Work directory does not exist: " + config.workDir + ". Use init() to create new beacon.");
  }

  config_.workDir = config.workDir;

  // Mount the ledger using Validator's mountLedger function
  std::string ledgerPath = config.workDir + "/ledger";
  log().info << "Mounting ledger at: " << ledgerPath;

  // Mount the ledger
  auto ledgerMountResult = mountLedger(ledgerPath);
  if (!ledgerMountResult) {
    return Error(3, "Failed to mount ledger: " + ledgerMountResult.error().message);
  }

  auto loadResult = loadFromLedger(0);
  if (!loadResult) {
    return Error(3, "Failed to load data from ledger: " + loadResult.error().message);
  }

  uint64_t blockCount = loadResult.value();

  log().info << "Beacon mounted successfully";
  log().info << "  Loaded " << blockCount << " blocks from ledger";
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

void Beacon::refresh() {
  // Update stakeholders
  refreshStakeholders();
}

Beacon::Roe<void> Beacon::addBlock(const Ledger::ChainNode& block) {
  // Call base class implementation which validates and adds to chain/ledger
  auto result = Validator::doAddBlock(block, true);
  if (!result) {
    return Error(4, result.error().message);
  }
  return {};
}

// Private helper methods

Beacon::Roe<Ledger::ChainNode> Beacon::createGenesisBlock(const BlockChainConfig& config, const InitKeyConfig& key) const {
  // Roles of genesis block:
  // 1. Mark initial checkpoint with blockchain parameters
  // 2. Create native token genesis wallet with zero balance
  // 3. Create first native token wallet that has stake to bootstrap consensus
  log().info << "Creating genesis block";

  // key.genesis/fee/reserve are KeyPairs; use publicKey for checkpoint, privateKey for signing
  SystemCheckpoint systemCheckpoint;
  systemCheckpoint.config = config;
  systemCheckpoint.genesis.balance = 0; // Native token (ID ID_GENESIS) with zero balance
  for (const auto& kp : key.genesis) {
    systemCheckpoint.genesis.publicKeys.push_back(kp.publicKey);
  }
  systemCheckpoint.genesis.minSignatures = key.genesis.size();
  systemCheckpoint.genesis.meta = "Native token genesis wallet";
  systemCheckpoint.fee.balance = 0; // Fee wallet (ID ID_FEE) with zero balance
  for (const auto& kp : key.fee) {
    systemCheckpoint.fee.publicKeys.push_back(kp.publicKey);
  }
  systemCheckpoint.fee.minSignatures = key.fee.size();
  systemCheckpoint.fee.meta = "Wallet for transaction fees";
  systemCheckpoint.reserve.balance = 0; // Reserve wallet (ID ID_RESERVE) with zero balance
  for (const auto& kp : key.reserve) {
    systemCheckpoint.reserve.publicKeys.push_back(kp.publicKey);
  }
  systemCheckpoint.reserve.minSignatures = key.reserve.size();
  systemCheckpoint.reserve.meta = "Native token reserve wallet";

  // Create genesis block with checkpoint transaction containing SystemCheckpoint
  Ledger::ChainNode genesisBlock;
  genesisBlock.block.index = 0;
  genesisBlock.block.timestamp = config.genesisTime;
  genesisBlock.block.previousHash = "0";
  genesisBlock.block.nonce = 0;
  genesisBlock.block.slot = 0;
  genesisBlock.block.slotLeader = 0;

  // Create checkpoint transaction with SystemCheckpoint
  Ledger::Transaction checkpointTx;
  checkpointTx.type = Ledger::Transaction::T_CHECKPOINT;
  checkpointTx.tokenId = AccountBuffer::ID_GENESIS; // Native token
  checkpointTx.fromWalletId = AccountBuffer::ID_GENESIS;     // genesis wallet ID
  checkpointTx.toWalletId = AccountBuffer::ID_GENESIS;       // genesis wallet ID
  checkpointTx.amount = 0;
  checkpointTx.fee = 0;
  // Serialize SystemCheckpoint to transaction metadata
  checkpointTx.meta = systemCheckpoint.ltsToString();

  // Initial transaction to fund reserve wallet and kickstart staking
  Ledger::Transaction reserveTx;
  reserveTx.type = Ledger::Transaction::T_DEFAULT;
  reserveTx.tokenId = AccountBuffer::ID_GENESIS;             // Native token
  reserveTx.fromWalletId = AccountBuffer::ID_GENESIS;        // genesis wallet ID
  reserveTx.toWalletId = AccountBuffer::ID_RESERVE;          // reserve wallet ID
  reserveTx.amount = AccountBuffer::INITIAL_TOKEN_SUPPLY - config.minFeePerTransaction;     // initial supply minus fee
  reserveTx.fee = config.minFeePerTransaction; // minimal fee
  reserveTx.meta = "Initial reserve funding for staking";

  // Sign with KeyPairs (key.genesis holds KeyPairs)
  Ledger::SignedData<Ledger::Transaction> signedTx;
  signedTx.obj = checkpointTx;
  std::string message = utl::binaryPack(checkpointTx);
  for (const auto& kp : key.genesis) {
    auto result = utl::ed25519Sign(kp.privateKey, message);
    if (!result) {
      return Error(18, "Failed to sign checkpoint transaction: " + result.error().message);
    }
    signedTx.signatures.push_back(*result);
  }
  genesisBlock.block.signedTxes.push_back(signedTx);

  signedTx.obj = reserveTx;
  signedTx.signatures.clear();
  message = utl::binaryPack(reserveTx);
  for (const auto& kp : key.genesis) {
    auto result = utl::ed25519Sign(kp.privateKey, message);
    if (!result) {
      return Error(18, "Failed to sign reserve transaction: " + result.error().message);
    }
    signedTx.signatures.push_back(*result);
  }
  genesisBlock.block.signedTxes.push_back(signedTx);

  // Calculate hash for genesis block
  genesisBlock.hash = calculateHash(genesisBlock.block);

  log().debug << "Genesis block created with hash: " << genesisBlock.hash;

  return genesisBlock;
}

} // namespace pp
