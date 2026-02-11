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

Beacon::Beacon() {
  redirectLogger("Beacon");
  validator_.redirectLogger(log().getFullName() + ".Chain");
}

uint64_t Beacon::getLastCheckpointId() const {
  return validator_.getLastCheckpointId();
}

uint64_t Beacon::getCurrentCheckpointId() const {
  return validator_.getCurrentCheckpointId();
}

uint64_t Beacon::getNextBlockId() const {
  return validator_.getNextBlockId();
}

uint64_t Beacon::getCurrentSlot() const {
  return validator_.getCurrentSlot();
}

uint64_t Beacon::getCurrentEpoch() const {
  return validator_.getCurrentEpoch();
}

std::vector<consensus::Stakeholder> Beacon::getStakeholders() const {
  return validator_.getStakeholders();
}

Beacon::Roe<Ledger::ChainNode> Beacon::getBlock(uint64_t blockId) const {
  auto result = validator_.getBlock(blockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Beacon::Roe<Client::UserAccount> Beacon::getAccount(uint64_t accountId) const {
  auto result = validator_.getAccount(accountId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

std::string Beacon::calculateHash(const Ledger::Block& block) const {
  return validator_.calculateHash(block);
}

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
  validator_.initConsensus(consensusConfig);
  
  // Initialize ledger
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = config.workDir + "/" + DIR_LEDGER;
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = validator_.initLedger(ledgerConfig);
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
             << Chain::GenesisAccountMeta::VERSION << ")";

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

  // Mount the ledger using Chain's mountLedger function
  std::string ledgerPath = config.workDir + "/" + DIR_LEDGER;
  log().info << "Mounting ledger at: " << ledgerPath;

  // Mount the ledger
  auto ledgerMountResult = validator_.mountLedger(ledgerPath);
  if (!ledgerMountResult) {
    return Error(3, "Failed to mount ledger: " + ledgerMountResult.error().message);
  }

  auto loadResult = validator_.loadFromLedger(0);
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
  validator_.refreshStakeholders();
}

Beacon::Roe<void> Beacon::addBlock(const Ledger::ChainNode& block) {
  // Call base class implementation which validates and adds to chain/ledger
  auto result = validator_.addBlock(block, true);
  if (!result) {
    return Error(4, result.error().message);
  }
  return {};
}

// Private helper methods

Beacon::Roe<Ledger::ChainNode> Beacon::createGenesisBlock(const Chain::BlockChainConfig& config, const InitKeyConfig& key) const {
  // Roles of genesis block:
  // 1. Mark initial checkpoint with blockchain parameters
  // 2. Create native token genesis wallet with zero balance
  // 3. Create first native token wallet that has stake to bootstrap consensus
  log().info << "Creating genesis block";

  // Create genesis block
  Ledger::ChainNode genesisBlock;
  genesisBlock.block.index = 0;
  genesisBlock.block.timestamp = config.genesisTime;
  genesisBlock.block.previousHash = "0";
  genesisBlock.block.nonce = 0;
  genesisBlock.block.slot = 0;
  genesisBlock.block.slotLeader = 0;

  // key.genesis/fee/reserve are KeyPairs; use publicKey for checkpoint, privateKey for signing
  Chain::GenesisAccountMeta gm;
  gm.config = config;

  gm.genesis.wallet.mBalances[AccountBuffer::ID_GENESIS] = 0;
  for (const auto& kp : key.genesis) {
    gm.genesis.wallet.publicKeys.push_back(kp.publicKey);
  }
  gm.genesis.wallet.minSignatures = key.genesis.size();
  gm.genesis.meta = "Native token genesis wallet";

  // First transaction: GenesisAccountMeta
  Ledger::SignedData<Ledger::Transaction> signedTx;
  signedTx.obj.type = Ledger::Transaction::T_GENESIS;
  signedTx.obj.tokenId = AccountBuffer::ID_GENESIS; // Native token
  signedTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;     // genesis wallet ID
  signedTx.obj.toWalletId = AccountBuffer::ID_GENESIS;       // genesis wallet ID
  signedTx.obj.amount = 0;                                   // no funding needed
  signedTx.obj.fee = 0;                                      // no fee wallet yet
  // Serialize GenesisAccountMeta to transaction metadata
  signedTx.obj.meta = gm.ltsToString();

  std::string message = utl::binaryPack(signedTx.obj);
  for (const auto& kp : key.genesis) {
    auto result = utl::ed25519Sign(kp.privateKey, message);
    if (!result) {
      return Error(18, "Failed to sign checkpoint transaction: " + result.error().message);
    }
    signedTx.signatures.push_back(*result);
  }
  genesisBlock.block.signedTxes.push_back(signedTx);


  // Second transaction: Create fee wallet and fund it with initial fee
  Client::UserAccount feeAccount;
  feeAccount.wallet.mBalances[AccountBuffer::ID_GENESIS] = 0;
  for (const auto& kp : key.fee) {
    feeAccount.wallet.publicKeys.push_back(kp.publicKey);
  }
  feeAccount.wallet.minSignatures = key.fee.size();
  feeAccount.meta = "Wallet for transaction fees";
  signedTx = {};
  signedTx.obj.type = Ledger::Transaction::T_NEW_USER;
  signedTx.obj.tokenId = AccountBuffer::ID_GENESIS;       // Native token
  signedTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;  // genesis wallet ID
  signedTx.obj.toWalletId = AccountBuffer::ID_FEE;        // fee wallet ID
  signedTx.obj.amount = 0;                                // no initial funding
  signedTx.obj.fee = 0;                                   // minimal fee
  signedTx.obj.meta = feeAccount.ltsToString();

  message = utl::binaryPack(signedTx.obj);
  for (const auto& kp : key.genesis) {
    auto result = utl::ed25519Sign(kp.privateKey, message);
    if (!result) {
      return Error(18, "Failed to sign fee transaction: " + result.error().message);
    }
    signedTx.signatures.push_back(*result);
  }
  genesisBlock.block.signedTxes.push_back(signedTx);


  Client::UserAccount reserveAccount;
  reserveAccount.wallet.mBalances[AccountBuffer::ID_GENESIS] = AccountBuffer::INITIAL_TOKEN_SUPPLY - config.minFeePerTransaction;
  for (const auto& kp : key.reserve) {
    reserveAccount.wallet.publicKeys.push_back(kp.publicKey);
  }
  reserveAccount.wallet.minSignatures = key.reserve.size();
  reserveAccount.meta = "Native token reserve wallet";

  // Third transaction: Initial reserve funding for staking
  signedTx = {};
  signedTx.obj.type = Ledger::Transaction::T_NEW_USER;
  signedTx.obj.tokenId = AccountBuffer::ID_GENESIS;             // Native token
  signedTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;        // genesis wallet ID
  signedTx.obj.toWalletId = AccountBuffer::ID_RESERVE;          // reserve wallet ID
  signedTx.obj.amount = AccountBuffer::INITIAL_TOKEN_SUPPLY - config.minFeePerTransaction;     // initial supply minus fee
  signedTx.obj.fee = config.minFeePerTransaction; // minimal fee
  signedTx.obj.meta = reserveAccount.ltsToString();

  message = utl::binaryPack(signedTx.obj);
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
