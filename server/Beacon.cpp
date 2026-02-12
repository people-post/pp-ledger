#include "Beacon.h"
#include "../client/Client.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include "../lib/BinaryPack.hpp"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

namespace {

Client::UserAccount makeUserAccountFromKeys(const std::vector<utl::Ed25519KeyPair>& keys,
    int64_t balance, const std::string& meta) {
  Client::UserAccount account;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  for (const auto& kp : keys) {
    account.wallet.publicKeys.push_back(kp.publicKey);
  }
  account.wallet.minSignatures = keys.size();
  account.meta = meta;
  return account;
}

}  // namespace

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
  for (const auto& kp : recycle) {
    j["recycle"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)}, {"privateKey", utl::hexEncode(kp.privateKey)}});
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

Beacon::Roe<void> Beacon::signWithGenesisKeys(Ledger::SignedData<Ledger::Transaction>& signedTx,
    const std::vector<utl::Ed25519KeyPair>& genesisKeys,
    const std::string& errorContext) const {
  std::string message = utl::binaryPack(signedTx.obj);
  for (const auto& kp : genesisKeys) {
    auto result = utl::ed25519Sign(kp.privateKey, message);
    if (!result) {
      return Error(18, "Failed to sign " + errorContext + ": " + result.error().message);
    }
    signedTx.signatures.push_back(*result);
  }
  return {};
}

Beacon::Roe<Ledger::ChainNode> Beacon::createGenesisBlock(const Chain::BlockChainConfig& config, const InitKeyConfig& key) const {
  // Roles of genesis block:
  // 1. Mark initial checkpoint with blockchain parameters
  // 2. Create fee, reserve, and recycle accounts
  log().info << "Creating genesis block";

  Ledger::ChainNode genesisBlock;
  genesisBlock.block.index = 0;
  genesisBlock.block.timestamp = config.genesisTime;
  genesisBlock.block.previousHash = "0";
  genesisBlock.block.nonce = 0;
  genesisBlock.block.slot = 0;
  genesisBlock.block.slotLeader = 0;

  Chain::GenesisAccountMeta gm;
  gm.config = config;
  gm.genesis = makeUserAccountFromKeys(key.genesis, 0, "Native token genesis wallet");

  // First transaction: GenesisAccountMeta
  Ledger::SignedData<Ledger::Transaction> signedTx;
  signedTx.obj.type = Ledger::Transaction::T_GENESIS;
  signedTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  signedTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  signedTx.obj.toWalletId = AccountBuffer::ID_GENESIS;
  signedTx.obj.amount = 0;
  signedTx.obj.fee = 0;
  signedTx.obj.meta = gm.ltsToString();
  auto roeGenesis = signWithGenesisKeys(signedTx, key.genesis, "checkpoint transaction");
  if (!roeGenesis) {
    return roeGenesis.error();
  }
  genesisBlock.block.signedTxes.push_back(signedTx);

  // Second transaction: Create fee wallet
  auto feeAccount = makeUserAccountFromKeys(key.fee, 0, "Wallet for transaction fees");
  signedTx = {};
  signedTx.obj.type = Ledger::Transaction::T_NEW_USER;
  signedTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  signedTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  signedTx.obj.toWalletId = AccountBuffer::ID_FEE;
  signedTx.obj.amount = 0;
  signedTx.obj.fee = 0;
  signedTx.obj.meta = feeAccount.ltsToString();
  auto roeFee = signWithGenesisKeys(signedTx, key.genesis, "fee transaction");
  if (!roeFee) {
    return roeFee.error();
  }
  genesisBlock.block.signedTxes.push_back(signedTx);

  // Third transaction: Create reserve wallet with initial stake
  int64_t reserveAmount = static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY - config.minFeePerTransaction);
  auto reserveAccount = makeUserAccountFromKeys(key.reserve, reserveAmount, "Native token reserve wallet");
  signedTx = {};
  signedTx.obj.type = Ledger::Transaction::T_NEW_USER;
  signedTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  signedTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  signedTx.obj.toWalletId = AccountBuffer::ID_RESERVE;
  signedTx.obj.amount = reserveAmount;
  signedTx.obj.fee = static_cast<int64_t>(config.minFeePerTransaction);
  signedTx.obj.meta = reserveAccount.ltsToString();
  auto roeReserve = signWithGenesisKeys(signedTx, key.genesis, "reserve transaction");
  if (!roeReserve) {
    return roeReserve.error();
  }
  genesisBlock.block.signedTxes.push_back(signedTx);

  // Fourth transaction: Create recycle account (sink for write-off balances)
  auto recycleAccount = makeUserAccountFromKeys(key.recycle, 0, "Account for recycling write-off balances");
  signedTx = {};
  signedTx.obj.type = Ledger::Transaction::T_NEW_USER;
  signedTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  signedTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  signedTx.obj.toWalletId = AccountBuffer::ID_RECYCLE;
  signedTx.obj.amount = 0;
  signedTx.obj.fee = static_cast<int64_t>(config.minFeePerTransaction);
  signedTx.obj.meta = recycleAccount.ltsToString();
  auto roeRecycle = signWithGenesisKeys(signedTx, key.genesis, "recycle transaction");
  if (!roeRecycle) {
    return roeRecycle.error();
  }
  genesisBlock.block.signedTxes.push_back(signedTx);

  genesisBlock.hash = calculateHash(genesisBlock.block);
  log().debug << "Genesis block created with hash: " << genesisBlock.hash;
  return genesisBlock;
}

} // namespace pp
