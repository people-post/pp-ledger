#include "Beacon.h"
#include "../chain/TxFees.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "lib/common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace pp {

namespace {

Client::UserAccount
makeUserAccountFromKeys(const std::vector<utl::Ed25519KeyPair> &keys,
                        int64_t balance, const std::string &meta) {
  Client::UserAccount account;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  for (const auto &kp : keys) {
    account.wallet.publicKeys.push_back(kp.publicKey);
  }
  account.wallet.minSignatures = keys.size();
  account.wallet.keyType = Crypto::TK_ED25519;
  account.meta = meta;
  return account;
}

} // namespace

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Beacon::InitConfig &config) {
  os << "InitConfig{workDir=\"" << config.workDir << "\", "
     << "chain=" << config.chain << "}";
  return os;
}

std::ostream &operator<<(std::ostream &os, const Beacon::MountConfig &config) {
  os << "MountConfig{workDir=\"" << config.workDir << "\"}";
  return os;
}

Beacon::Beacon() {
  redirectLogger("Beacon");
  chain_.redirectLogger(log().getFullName() + ".Chain");
}

Chain::Checkpoint Beacon::getCheckpoint() const {
  return chain_.getCheckpoint();
}

uint64_t Beacon::getNextBlockId() const { return chain_.getNextBlockId(); }

uint64_t Beacon::getCurrentSlot() const { return chain_.getCurrentSlot(); }

uint64_t Beacon::getCurrentEpoch() const { return chain_.getCurrentEpoch(); }

std::vector<consensus::Stakeholder> Beacon::getStakeholders() const {
  return chain_.getStakeholders();
}

Beacon::Roe<Ledger::ChainNode> Beacon::readBlock(uint64_t blockId) const {
  auto result = chain_.readBlock(blockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Beacon::Roe<Client::UserAccount> Beacon::getAccount(uint64_t accountId) const {
  auto result = chain_.getAccount(accountId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

std::string Beacon::calculateHash(const Ledger::Block &block) const {
  return chain_.calculateHash(block);
}

Beacon::Roe<std::vector<Ledger::Record>>
Beacon::findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const {
  auto result = chain_.findTransactionsByWalletId(walletId, ioBlockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Beacon::Roe<Ledger::Record>
Beacon::findTransactionByIndex(uint64_t txIndex) const {
  auto result = chain_.findTransactionByIndex(txIndex);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

nlohmann::json Beacon::InitKeyConfig::toJson() const {
  nlohmann::json j;
  for (const auto &kp : genesis) {
    j["genesis"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)},
                            {"privateKey", utl::hexEncode(kp.privateKey)}});
  }
  for (const auto &kp : fee) {
    j["fee"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)},
                        {"privateKey", utl::hexEncode(kp.privateKey)}});
  }
  for (const auto &kp : reserve) {
    j["reserve"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)},
                            {"privateKey", utl::hexEncode(kp.privateKey)}});
  }
  for (const auto &kp : recycle) {
    j["recycle"].push_back({{"publicKey", utl::hexEncode(kp.publicKey)},
                            {"privateKey", utl::hexEncode(kp.privateKey)}});
  }
  return j;
}

Beacon::Roe<void> Beacon::init(const InitConfig &config) {
  log().info << "Initializing Beacon";
  log().debug << "Init config: " << config;

  // Verify work directory does NOT exist (fresh initialization)
  if (std::filesystem::exists(config.workDir)) {
    return Error("Work directory already exists: " + config.workDir +
                 ". Use mount() to load existing beacon.");
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
  chain_.initConsensus(consensusConfig);

  // Initialize ledger
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = config.workDir + "/" + DIR_LEDGER;
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = chain_.initLedger(ledgerConfig);
  if (!ledgerResult) {
    return Error(2, "Failed to initialize ledger: " +
                        ledgerResult.error().message);
  }

  config_.workDir = config.workDir;
  auto chainConfig = config.chain;
  chainConfig.genesisTime = consensusConfig.genesisTime;

  // Create and add genesis block
  auto genesisBlockResult = createGenesisBlock(chainConfig, config.key);
  if (!genesisBlockResult) {
    return Error(2, "Failed to create genesis block: " +
                        genesisBlockResult.error().message);
  }
  auto genesisBlock = genesisBlockResult.value();
  auto addBlockResult = addBlock(genesisBlock);
  if (!addBlockResult) {
    return Error(2, "Failed to add genesis block: " +
                        addBlockResult.error().message);
  }

  log().info << "Genesis block created with checkpoint transaction (version "
             << Chain::GenesisAccountMeta::VERSION << ")";

  log().info << "Beacon initialized successfully";
  log().info << "  Genesis time: " << consensusConfig.genesisTime;
  log().info << "  Time offset: " << consensusConfig.timeOffset;
  log().info << "  Slot duration: " << consensusConfig.slotDuration;
  log().info << "  Slots per epoch: " << consensusConfig.slotsPerEpoch;
  log().info << "  Max custom meta size: "
             << chainConfig.maxCustomMetaSize;
  log().info << "  Max transactions per block: "
             << chainConfig.maxTransactionsPerBlock;
  log().info << "  Free custom meta size: "
             << chainConfig.freeCustomMetaSize;
  log().info << "  Min fee coefficients: "
             << "[" << utl::join(chainConfig.minFeeCoefficients, ", ")
             << "]";
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

Beacon::Roe<void> Beacon::mount(const MountConfig &config) {
  log().info << "Mounting Beacon at: " << config.workDir;
  log().debug << "Mount config: " << config;

  // Verify work directory exists (loading existing state)
  if (!std::filesystem::exists(config.workDir)) {
    return Error(3, "Work directory does not exist: " + config.workDir +
                        ". Use init() to create new beacon.");
  }

  config_.workDir = config.workDir;

  // Mount the ledger using Chain's mountLedger function
  std::string ledgerPath = config.workDir + "/" + DIR_LEDGER;
  log().info << "Mounting ledger at: " << ledgerPath;

  // Mount the ledger
  auto ledgerMountResult = chain_.mountLedger(ledgerPath);
  if (!ledgerMountResult) {
    return Error(3, "Failed to mount ledger: " +
                        ledgerMountResult.error().message);
  }

  auto loadResult = chain_.loadFromLedger(0);
  if (!loadResult) {
    return Error(3, "Failed to load data from ledger: " +
                        loadResult.error().message);
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
  chain_.refreshStakeholders();
}

Beacon::Roe<void> Beacon::addBlock(const Ledger::ChainNode &block) {
  // Call base class implementation which validates and adds to chain/ledger
  auto result = chain_.addBlock(block);
  if (!result) {
    return Error(4, result.error().message);
  }
  return {};
}

// Private helper methods

Beacon::Roe<void>
Beacon::signWithGenesisKeys(Ledger::Record &record,
                            const std::vector<utl::Ed25519KeyPair> &genesisKeys,
                            const std::string &errorContext) const {
  const std::string &message = record.data;
  for (const auto &kp : genesisKeys) {
    auto result = utl::ed25519Sign(kp.privateKey, message);
    if (!result) {
      return Error(18, "Failed to sign " + errorContext + ": " +
                           result.error().message);
    }
    record.signatures.push_back(*result);
  }
  return {};
}

Beacon::Roe<Ledger::ChainNode>
Beacon::createGenesisBlock(const Chain::BlockChainConfig &config,
                           const InitKeyConfig &key) const {
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
  gm.genesis =
      makeUserAccountFromKeys(key.genesis, 0, "Native token genesis wallet");

  // First transaction: GenesisAccountMeta
  Ledger::TxGenesis txGenesis;
  txGenesis.fee = 0;
  txGenesis.meta = gm.ltsToString();

  Ledger::Record rec;
  rec.type = Ledger::T_GENESIS;
  rec.data = utl::binaryPack(txGenesis);
  rec.signatures = {};
  auto roeGenesis =
      signWithGenesisKeys(rec, key.genesis, "checkpoint transaction");
  if (!roeGenesis) {
    return roeGenesis.error();
  }
  genesisBlock.block.records.push_back(rec);

  // Second transaction: Create fee wallet
  auto feeAccount =
      makeUserAccountFromKeys(key.fee, 0, "Wallet for transaction fees");

  Ledger::TxNewUser txFee;
  txFee.fromWalletId = AccountBuffer::ID_GENESIS;
  txFee.toWalletId = AccountBuffer::ID_FEE;
  txFee.amount = 0;
  txFee.meta = feeAccount.ltsToString();
  const Ledger::TypedTx feeTypedTx(txFee);
  auto feeWalletFeeResult =
      chain_tx::calculateMinimumFeeForTransaction(config, feeTypedTx);
  if (!feeWalletFeeResult) {
    return Error(2, "Failed to calculate fee-wallet transaction fee: " +
                        feeWalletFeeResult.error().message);
  }
  const int64_t feeWalletFee =
      static_cast<int64_t>(feeWalletFeeResult.value());
  txFee.fee = feeWalletFee;

  rec = {};
  rec.type = Ledger::T_NEW_USER;
  rec.data = utl::binaryPack(txFee);
  rec.signatures = {};
  auto roeFee = signWithGenesisKeys(rec, key.genesis, "fee transaction");
  if (!roeFee) {
    return roeFee.error();
  }
  genesisBlock.block.records.push_back(rec);

  // Third transaction: Create reserve wallet with initial stake
  auto reserveAccount =
      makeUserAccountFromKeys(key.reserve, 0, "Native token reserve wallet");
  auto recycleAccount = makeUserAccountFromKeys(
      key.recycle, 0, "Account for recycling write-off balances");

  auto getNonFreeMetaSize = [&](size_t customMetaSize) -> uint64_t {
    return customMetaSize > config.freeCustomMetaSize
               ? static_cast<uint64_t>(customMetaSize) -
                     config.freeCustomMetaSize
               : 0ULL;
  };

  auto recycleFeeResult = chain_tx::calculateMinimumFeeFromNonFreeMetaSize(
      config, getNonFreeMetaSize(recycleAccount.meta.size()));
  if (!recycleFeeResult) {
    return Error(2, "Failed to calculate recycle fee: " +
                      recycleFeeResult.error().message);
  }
  const int64_t recycleFee = static_cast<int64_t>(recycleFeeResult.value());

  int64_t reserveAmount =
      static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY);
  int64_t reserveFee = 0;
  for (int i = 0; i < 2; ++i) {
    reserveAccount.wallet.mBalances[AccountBuffer::ID_GENESIS] = reserveAmount;
    auto reserveFeeResult = chain_tx::calculateMinimumFeeFromNonFreeMetaSize(
        config, getNonFreeMetaSize(reserveAccount.meta.size()));
    if (!reserveFeeResult) {
      return Error(2, "Failed to calculate reserve fee: " +
                          reserveFeeResult.error().message);
    }
    reserveFee = static_cast<int64_t>(reserveFeeResult.value());

    const int64_t updatedReserveAmount =
        static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY) -
        feeWalletFee - reserveFee - recycleFee;
    if (updatedReserveAmount < 0) {
      return Error(2, "Initial token supply is insufficient for genesis fees");
    }
    if (updatedReserveAmount == reserveAmount) {
      break;
    }
    reserveAmount = updatedReserveAmount;
  }
  reserveAccount.wallet.mBalances[AccountBuffer::ID_GENESIS] = reserveAmount;

  Ledger::TxNewUser txReserve;
  txReserve.fromWalletId = AccountBuffer::ID_GENESIS;
  txReserve.toWalletId = AccountBuffer::ID_RESERVE;
  txReserve.amount = static_cast<uint64_t>(reserveAmount);
  txReserve.fee = static_cast<uint64_t>(reserveFee);
  txReserve.meta = reserveAccount.ltsToString();

  rec = {};
  rec.type = Ledger::T_NEW_USER;
  rec.data = utl::binaryPack(txReserve);
  rec.signatures = {};
  auto roeReserve =
      signWithGenesisKeys(rec, key.genesis, "reserve transaction");
  if (!roeReserve) {
    return roeReserve.error();
  }
  genesisBlock.block.records.push_back(rec);

  // Fourth transaction: Create recycle account (sink for write-off balances)
  Ledger::TxNewUser txRecycle;
  txRecycle.fromWalletId = AccountBuffer::ID_GENESIS;
  txRecycle.toWalletId = AccountBuffer::ID_RECYCLE;
  txRecycle.amount = 0;
  txRecycle.fee = static_cast<uint64_t>(recycleFee);
  txRecycle.meta = recycleAccount.ltsToString();

  rec = {};
  rec.type = Ledger::T_NEW_USER;
  rec.data = utl::binaryPack(txRecycle);
  rec.signatures = {};
  auto roeRecycle =
      signWithGenesisKeys(rec, key.genesis, "recycle transaction");
  if (!roeRecycle) {
    return roeRecycle.error();
  }
  genesisBlock.block.records.push_back(rec);

  genesisBlock.hash = calculateHash(genesisBlock.block);
  log().debug << "Genesis block created with hash: " << genesisBlock.hash;
  return genesisBlock;
}

} // namespace pp
