#include "Beacon.h"
#include "../chain/AccountPolicy.h"
#include "../chain/TxFees.h"
#include "../client/AccountAttachment.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <set>

namespace pp {

namespace {

Client::UserAccount
makeUserAccountFromKeys(const Beacon::AccountKeys &keys,
                        int64_t balance, const std::string &meta) {
  Client::UserAccount account;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  account.wallet.publicKeys = keys.publicKeys;
  account.wallet.minSignatures =
      keys.minSignatures != 0
          ? keys.minSignatures
          : static_cast<uint8_t>(std::min<size_t>(keys.publicKeys.size(), std::numeric_limits<uint8_t>::max()));
  account.wallet.keyType = Crypto::TK_ML_DSA_65;
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

std::string Beacon::getNetworkId() const {
  return chain_.getNetworkId();
}

Beacon::Roe<void> Beacon::verifyMinerRegistration(const Client::MinerInfo &miner,
                                                 const std::string &networkId) const {
  auto signed_ = chain_.verifyAccountSignatures(miner.id, miner.signingMessage(networkId),
                                                miner.signatures);
  if (!signed_) {
    return Error(signed_.error().code, "Registration not signed by miner " + std::to_string(miner.id) + ": " +
                                           signed_.error().message);
  }
  return {};
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
  consensus::SlotCommittee::Config consensusConfig;
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
  auto genesisBlockResult = createGenesisBlock(chainConfig, config.key, config.miners);
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
                            const std::vector<std::string> &genesisSigners,
                            const std::string &networkId,
                            const std::string &errorContext) const {
  const std::string message = record.signingMessage(networkId);
  for (const auto &privateKey : genesisSigners) {
    auto result = utl::mlDsaSign(privateKey, message);
    if (!result) {
      return Error(18, "Failed to sign " + errorContext + ": " +
                           result.error().message);
    }
    record.signatures.push_back(*result);
  }
  return {};
}

Beacon::Roe<void> Beacon::checkInitKeys(const InitKeyConfig &key) const {
  const std::pair<const char *, const AccountKeys *> accounts[] = {
      {"genesis", &key.genesis}, {"fee", &key.fee}, {"reserve", &key.reserve}, {"recycle", &key.recycle}};
  for (const auto &[name, keys] : accounts) {
    if (keys->publicKeys.empty()) {
      return Error(2, std::string("System account '") + name + "' needs public keys");
    }
    for (const auto &pk : keys->publicKeys) {
      if (pk.size() != utl::kMlDsaPublicKeyBytes) {
        return Error(2, std::string("System account '") + name + "' has a malformed public key");
      }
    }
    if (keys->minSignatures > keys->publicKeys.size()) {
      return Error(2, std::string("System account '") + name + "' minSignatures exceeds its key count");
    }
  }
  // The genesis (admin) account is M-of-N from the start: the same shape its
  // renewals and config updates are held to (at least 3 keys, 2 signatures).
  Client::Wallet genesisWallet;
  genesisWallet.publicKeys = key.genesis.publicKeys;
  genesisWallet.minSignatures =
      key.genesis.minSignatures != 0 ? key.genesis.minSignatures
                                     : static_cast<uint8_t>(std::min<size_t>(key.genesis.publicKeys.size(), 255));
  if (auto shape = chain_tx::validateGenesisWalletShape(genesisWallet); !shape) {
    return Error(2, "systemAccounts.genesis: " + shape.error().message);
  }

  // Each signer must be a distinct genesis key: sign a probe and match it.
  const std::string probe = "pp-ledger/genesis-signer-probe/v1";
  std::set<size_t> matched;
  for (const auto &privateKey : key.genesisSigners) {
    auto sig = utl::mlDsaSign(privateKey, probe);
    if (!sig) {
      return Error(2, "Invalid genesis signing key: " + sig.error().message);
    }
    bool found = false;
    for (size_t i = 0; i < key.genesis.publicKeys.size() && !found; ++i) {
      if (utl::mlDsaVerify(key.genesis.publicKeys[i], probe, sig.value())) {
        found = matched.insert(i).second;
        if (!found) {
          return Error(2, "The same genesis signing key was given twice");
        }
      }
    }
    if (!found) {
      return Error(2, "A genesis signing key does not match any genesis public key");
    }
  }
  const size_t needed =
      key.genesis.minSignatures != 0 ? key.genesis.minSignatures : key.genesis.publicKeys.size();
  if (matched.size() < needed) {
    return Error(2, "Genesis needs " + std::to_string(needed) + " signing key(s) (--genesis-key), got " +
                        std::to_string(matched.size()));
  }
  return {};
}

Beacon::Roe<std::pair<std::vector<Ledger::Record>, int64_t>>
Beacon::createGenesisMinerRecords(const Chain::BlockChainConfig &config, const InitKeyConfig &key,
                                  const std::vector<GenesisMiner> &miners) {
  std::vector<Ledger::Record> records;
  int64_t total = 0;
  std::set<uint64_t> seen;
  for (const auto &miner : miners) {
    if (miner.id < AccountBuffer::ID_FIRST_ISSUED || miner.id >= AccountBuffer::ID_FIRST_USER) {
      return Error(2, "Genesis miner id " + std::to_string(miner.id) + " must be in the issued range [" +
                          std::to_string(AccountBuffer::ID_FIRST_ISSUED) + ", " +
                          std::to_string(AccountBuffer::ID_FIRST_USER) + ")");
    }
    if (!seen.insert(miner.id).second) {
      return Error(2, "Duplicate genesis miner id " + std::to_string(miner.id));
    }
    if (miner.publicKeys.empty() || miner.minSignatures == 0 || miner.minSignatures > miner.publicKeys.size()) {
      return Error(2, "Genesis miner " + std::to_string(miner.id) +
                          " needs public keys and 1 <= minSignatures <= key count");
    }
    if (miner.stake == 0 || miner.stake > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return Error(2, "Genesis miner " + std::to_string(miner.id) + " needs a positive stake");
    }
    Client::UserAccount account;
    account.wallet.mBalances[AccountBuffer::ID_GENESIS] = static_cast<int64_t>(miner.stake);
    account.wallet.publicKeys = miner.publicKeys;
    account.wallet.minSignatures = miner.minSignatures;
    account.wallet.keyType = Crypto::TK_ML_DSA_65;
    account.meta = AccountAttachment::emptySerialized();

    Ledger::TxNewUser tx;
    tx.fromWalletId = AccountBuffer::ID_GENESIS;
    tx.toWalletId = miner.id;
    tx.amount = miner.stake;
    tx.meta = account.ltsToString();
    auto fee = chain_.calculateMinimumFeeForTransaction(config, Ledger::TypedTx(tx));
    if (!fee) {
      return Error(2, "Failed to calculate genesis miner fee: " + fee.error().message);
    }
    tx.fee = fee.value();

    Ledger::Record rec;
    rec.type = Ledger::T_NEW_USER;
    rec.data = utl::binaryPack(tx);
    auto signedRec = signWithGenesisKeys(rec, key.genesisSigners, config.networkId, "genesis miner transaction");
    if (!signedRec) {
      return signedRec.error();
    }
    // All non-negative: overflow-checked sum (portable; no compiler builtins).
    const int64_t add = static_cast<int64_t>(miner.stake);
    const int64_t feeAmount = static_cast<int64_t>(tx.fee);
    if (add > std::numeric_limits<int64_t>::max() - total - feeAmount) {
      return Error(2, "Genesis miner stakes overflow");
    }
    total += add + feeAmount;
    records.push_back(std::move(rec));
  }
  return std::make_pair(std::move(records), total);
}

Beacon::Roe<Ledger::ChainNode>
Beacon::createGenesisBlock(const Chain::BlockChainConfig &config,
                           const InitKeyConfig &key,
                           const std::vector<GenesisMiner> &miners) {
  // Roles of genesis block:
  // 1. Mark initial checkpoint with blockchain parameters
  // 2. Create fee, reserve, and recycle accounts
  // 3. Create genesis miner accounts (their stake comes out of reserve)
  log().info << "Creating genesis block";
  if (auto keysOk = checkInitKeys(key); !keysOk) {
    return keysOk.error();
  }

  Ledger::ChainNode genesisBlock;
  genesisBlock.block.index = 0;
  genesisBlock.block.timestamp = config.genesisTime;
  genesisBlock.block.previousHash = utl::zeroHash();
  genesisBlock.block.slot = 0;
  genesisBlock.block.slotLeader = 0;
  genesisBlock.block.epoch = 0;

  Chain::GenesisAccountMeta gm;
  gm.config = config;
  gm.genesis =
      makeUserAccountFromKeys(key.genesis, 0, AccountAttachment::emptySerialized());

  // First transaction: GenesisAccountMeta
  Ledger::TxGenesis txGenesis;
  txGenesis.fee = 0;
  txGenesis.meta = gm.ltsToString();

  Ledger::Record rec;
  rec.type = Ledger::T_GENESIS;
  rec.data = utl::binaryPack(txGenesis);
  rec.signatures = {};
  auto roeGenesis =
      signWithGenesisKeys(rec, key.genesisSigners, config.networkId,
                          "checkpoint transaction");
  if (!roeGenesis) {
    return roeGenesis.error();
  }
  genesisBlock.block.records.push_back(rec);

  // Second transaction: Create fee wallet
  auto feeAccount =
      makeUserAccountFromKeys(key.fee, 0, AccountAttachment::emptySerialized());

  Ledger::TxNewUser txFee;
  txFee.fromWalletId = AccountBuffer::ID_GENESIS;
  txFee.toWalletId = AccountBuffer::ID_FEE;
  txFee.amount = 0;
  txFee.meta = feeAccount.ltsToString();
  const Ledger::TypedTx feeTypedTx(txFee);
  auto feeWalletFeeResult = chain_.calculateMinimumFeeForTransaction(config, feeTypedTx);
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
  auto roeFee = signWithGenesisKeys(rec, key.genesisSigners, config.networkId,
                                    "fee transaction");
  if (!roeFee) {
    return roeFee.error();
  }
  genesisBlock.block.records.push_back(rec);

  // Third transaction: Create reserve wallet with initial stake
  auto reserveAccount =
      makeUserAccountFromKeys(key.reserve, 0, AccountAttachment::emptySerialized());
  auto recycleAccount = makeUserAccountFromKeys(
      key.recycle, 0, AccountAttachment::emptySerialized());

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

  auto minerRecords = createGenesisMinerRecords(config, key, miners);
  if (!minerRecords) {
    return minerRecords.error();
  }
  const int64_t minerTotal = minerRecords.value().second;

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
        feeWalletFee - reserveFee - recycleFee - minerTotal;
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
      signWithGenesisKeys(rec, key.genesisSigners, config.networkId,
                          "reserve transaction");
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
      signWithGenesisKeys(rec, key.genesisSigners, config.networkId,
                          "recycle transaction");
  if (!roeRecycle) {
    return roeRecycle.error();
  }
  genesisBlock.block.records.push_back(rec);

  for (auto &minerRec : minerRecords.value().first) {
    genesisBlock.block.records.push_back(std::move(minerRec));
  }

  auto sealResult = chain_.sealBlock(genesisBlock);
  if (!sealResult) {
    return Error(19, "Failed to seal genesis block: " +
                         sealResult.error().message);
  }
  log().debug << "Genesis block created with hash: " << genesisBlock.hash;
  return genesisBlock;
}

} // namespace pp
