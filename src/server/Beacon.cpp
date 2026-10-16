#include "Beacon.h"
#include "NodeFile.h"
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

bool Beacon::isBeforeDeliveryDeadline(uint64_t slot) const { return chain_.isBeforeDeliveryDeadline(slot); }

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
  consensusConfig.timeOffsetMs = 0;
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
  // The beacon signs what it commits: every block is durable before it is signed (B009).
  chain_.setLedgerCommitPolicy(Ledger::CommitPolicy::everyBlock());

  config_.workDir = config.workDir;
  auto chainConfig = config.chain;
  chainConfig.genesisTime = consensusConfig.genesisTime;

  // Create and add genesis block
  auto authority = config.authority;
  authority.beacon.networkId = chainConfig.networkId;
  auto genesisBlockResult = createGenesisBlock(chainConfig, config.key, config.miners, authority);
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
  log().info << "  Time offset: " << consensusConfig.timeOffsetMs << " ms";
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
  if (auto rewound = applyPendingRewind(ledgerPath); !rewound) {
    return rewound;
  }
  log().info << "Mounting ledger at: " << ledgerPath;

  // Mount the ledger
  auto ledgerMountResult = chain_.mountLedger(ledgerPath);
  if (!ledgerMountResult) {
    return Error(3, "Failed to mount ledger: " +
                        ledgerMountResult.error().message);
  }
  // The beacon signs what it commits: every block is durable before it is signed (B009).
  chain_.setLedgerCommitPolicy(Ledger::CommitPolicy::everyBlock());

  auto loadResult = chain_.loadFromLedger(0);
  if (!loadResult) {
    return Error(3, "Failed to load data from ledger: " +
                        loadResult.error().message);
  }

  uint64_t blockCount = loadResult.value();

  certificates_ = std::make_unique<CertificateStore>(chain_, config.workDir + "/" + FILE_BEACON_CERT);
  certificates_->redirectLogger(log().getFullName() + ".CertificateStore");
  if (auto loaded = certificates_->load(); !loaded) {
    return Error(3, loaded.error().message);
  }
  equivocations_ = std::make_unique<EquivocationStore>(chain_, config.workDir + "/" + FILE_EQUIVOCATION);
  equivocations_->redirectLogger(log().getFullName() + ".EquivocationStore");
  if (auto loaded = equivocations_->load(); !loaded) {
    return Error(3, loaded.error().message);
  }

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
  if (const auto above = haltedAbove(); above && block.block.index > *above) {
    return Error(4, "Halted: beacon generation " + std::to_string(chain_.equivocation()->generation()) +
                        " signed two blocks at height " + std::to_string(*above) + " (equivocation)");
  }
  if (auto withinCutover = chain_.checkSeenCutover(block.block); !withinCutover) {
    return Error(4, withinCutover.error().message);
  }
  auto result = chain_.addBlock(block);
  if (!result) {
    return Error(4, result.error().message);
  }
  if (tipSigner_) {
    if (auto signed_ = signCommittedTip(); !signed_ && signed_.error().code != TipSigner::E_NOT_CERTIFIED) {
      log().error << "Block " << block.block.index << " committed but not signed: " << signed_.error().message;
    }
  }
  if (certificates_) {
    certificates_->refresh(); // a block may have committed it, or replaced the admins who signed it
  }
  if (equivocations_) {
    equivocations_->refresh(); // a certificate on chain may have resumed it
  }
  return {};
}

Beacon::Roe<void> Beacon::openTipSigning(const std::vector<utl::MlDsaKeyPair> &keys, bool acceptLedgerTip) {
  auto signer = std::make_unique<TipSigner>(config_.workDir + "/" + FILE_SIGNED_TIP, keys);
  signer->redirectLogger(log().getFullName() + ".TipSigner");
  if (auto loaded = signer->load(); !loaded) {
    return Error(5, loaded.error().message);
  }
  if (const auto &tip = signer->latest()) {
    if (auto held = checkLedgerHoldsSignedTip(*tip); !held) {
      return held;
    }
  } else if (auto unsigned_ = checkUnsignedLedger(*signer, acceptLedgerTip); !unsigned_) {
    return unsigned_;
  }
  tipSigner_ = std::move(signer);
  if (auto signed_ = signCommittedTip(); !signed_ && signed_.error().code != TipSigner::E_NOT_CERTIFIED) {
    tipSigner_.reset();
    return Error(5, signed_.error().message);
  }
  if (const auto &tip = tipSigner_->latest()) {
    log().info << "Signed tip: block " << tip->height << ", generation " << tip->generation;
  }
  return {};
}

std::optional<Client::SignedTip> Beacon::getSignedTip() const {
  return tipSigner_ ? tipSigner_->latest() : std::nullopt;
}

std::optional<uint64_t> Beacon::supersededBy() const {
  const auto authority = chain_.getAuthority();
  if (!tipSigner_ || !authority) {
    return std::nullopt;
  }
  // The latest certificate decides: one held out of band (not on chain yet) above the one in force.
  if (const auto &seen = chain_.seenCertificate()) {
    return tipSigner_->holdsKey(seen->publicKey) ? std::nullopt : std::optional<uint64_t>(seen->generation);
  }
  if (!tipSigner_->holdsKey(authority->beacon.publicKey)) {
    return authority->beacon.generation;
  }
  return std::nullopt;
}

std::optional<uint64_t> Beacon::takingOver() const {
  if (!tipSigner_) {
    return std::nullopt;
  }
  return takeoverFor([this](const std::string &publicKey) { return tipSigner_->holdsKey(publicKey); });
}

std::optional<uint64_t> Beacon::takeoverWith(const std::vector<utl::MlDsaKeyPair> &keys) const {
  return takeoverFor([&keys](const std::string &publicKey) {
    return std::any_of(keys.begin(), keys.end(),
                       [&publicKey](const utl::MlDsaKeyPair &key) { return key.publicKey == publicKey; });
  });
}

std::optional<uint64_t> Beacon::takeoverFor(const std::function<bool(const std::string &publicKey)> &holds) const {
  const auto authority = chain_.getAuthority();
  const auto &seen = chain_.seenCertificate();
  if (!authority || !seen || !holds(seen->publicKey) || holds(authority->beacon.publicKey)) {
    return std::nullopt; // a staged rotation holds the key in force too: it is not a takeover
  }
  return seen->generation;
}

Beacon::Roe<Chain::CertificateOffer> Beacon::offerCertificate(const Ledger::Record &record) {
  if (!certificates_) {
    return Error(6, "Beacon not mounted");
  }
  auto offered = certificates_->offer(record);
  if (!offered) {
    return Error(offered.error().code, offered.error().message);
  }
  if (offered.value() == Chain::CertificateOffer::Newer) {
    if (equivocations_) {
      equivocations_->refresh(); // a later generation resumes a halted one
    }
    if (const auto by = supersededBy()) {
      log().error << "Beacon generation " << *by << " (seen out of band) certifies none of this beacon's keys: "
                  << "it stands down and serves reads only";
    }
  }
  return offered.value();
}

std::optional<uint64_t> Beacon::haltedAbove() const {
  const auto &evidence = chain_.equivocation();
  return evidence ? std::optional<uint64_t>(evidence->height()) : std::nullopt;
}

Beacon::Roe<Chain::EquivocationOffer> Beacon::offerEquivocation(const Client::Equivocation &evidence) {
  if (!equivocations_) {
    return Error(6, "Beacon not mounted");
  }
  auto offered = equivocations_->offer(evidence);
  if (!offered) {
    return Error(offered.error().code, offered.error().message);
  }
  if (offered.value() == Chain::EquivocationOffer::Newer) {
    log().error << "Beacon generation " << evidence.generation() << " signed two blocks at height "
                << evidence.height() << ": this beacon commits nothing above it and serves reads only until "
                << "a later generation is certified";
  }
  return offered.value();
}

std::optional<Ledger::Record> Beacon::getCurrentCertificate() const {
  return chain_.currentCertificateRecord();
}

uint64_t Beacon::getGeneration() const {
  const auto authority = chain_.getAuthority();
  return authority ? authority->beacon.generation : 0;
}

Beacon::Roe<void> Beacon::checkLedgerHoldsSignedTip(const Client::SignedTip &tip) const {
  if (tip.networkId != getNetworkId()) {
    return Error(5, std::string(FILE_SIGNED_TIP) + " is for network '" + tip.networkId + "', not '" +
                        getNetworkId() + "'");
  }
  if (tip.height >= getNextBlockId()) {
    return Error(5, "The ledger ends at block " + std::to_string(getNextBlockId() - 1) +
                        ", below the last signed tip (block " + std::to_string(tip.height) +
                        "): restore the ledger; the beacon must not sign those heights again");
  }
  auto block = readBlock(tip.height);
  if (!block || block.value().hash != tip.blockHash) {
    return Error(5, "The ledger's block " + std::to_string(tip.height) +
                        " is not the one the beacon signed: restore the ledger it signed");
  }
  return {};
}

TipSigner::Roe<void> Beacon::signCommittedTip() {
  const auto authority = chain_.getAuthority();
  if (!authority) {
    return TipSigner::Error(TipSigner::E_IO, "Chain authority not loaded");
  }
  const uint64_t height = getNextBlockId() - 1;
  auto block = readBlock(height);
  if (!block) {
    return TipSigner::Error(TipSigner::E_IO, "Failed to read block " + std::to_string(height) + ": " +
                                                 block.error().message);
  }
  auto signed_ = tipSigner_->sign(authority->beacon, height, block.value().hash);
  if (!signed_ && signed_.error().code == TipSigner::E_NOT_CERTIFIED &&
      uncertifiedWarnedGeneration_ != authority->beacon.generation) {
    uncertifiedWarnedGeneration_ = authority->beacon.generation;
    if (const auto generation = takingOver()) {
      log().warning << "Taking over as beacon generation " << *generation << ": signs nothing until block "
                    << chain_.seenCertificate()->cutoverHeight + 1 << " commits its certificate";
    } else {
      log().error << signed_.error().message << ": this beacon stands down (stops committing, serves reads)";
    }
  }
  return signed_;
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
      {"genesis", &key.genesis},     {"reserve", &key.reserve}, {"registrar", &key.registrar},
      {"fee", &key.fee},             {"recycle", &key.recycle}};
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

Beacon::Roe<Ledger::Record>
Beacon::createSystemAccountRecord(const Chain::BlockChainConfig &config, const InitKeyConfig &key, uint64_t toId,
                                  const Client::UserAccount &account, uint64_t &fee, const std::string &label) {
  Ledger::TxNewUser tx;
  tx.fromWalletId = AccountBuffer::ID_GENESIS;
  tx.toWalletId = toId;
  auto it = account.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
  tx.amount = it == account.wallet.mBalances.end() ? 0 : static_cast<uint64_t>(it->second);
  tx.meta = account.ltsToString();
  if (fee == 0) {
    auto minFee = chain_.calculateMinimumFeeForTransaction(config, Ledger::TypedTx(tx));
    if (!minFee) {
      return Error(2, "Failed to calculate " + label + " fee: " + minFee.error().message);
    }
    fee = minFee.value();
  }
  tx.fee = fee;

  Ledger::Record rec;
  rec.type = Ledger::T_NEW_USER;
  rec.data = utl::binaryPack(tx);
  if (auto signedRec = signWithGenesisKeys(rec, key.genesisSigners, config.networkId, label); !signedRec) {
    return signedRec.error();
  }
  return rec;
}

Beacon::Roe<Ledger::Record> Beacon::createReserveRecord(const Chain::BlockChainConfig &config,
                                                        const InitKeyConfig &key, int64_t otherCosts) {
  // The fee bills the account's custom meta only, not its balance.
  Ledger::TxNewUser probe;
  probe.meta = makeUserAccountFromKeys(key.reserve, 0, AccountAttachment::emptySerialized()).ltsToString();
  auto feeRoe = chain_.calculateMinimumFeeForTransaction(config, Ledger::TypedTx(probe));
  if (!feeRoe) {
    return Error(2, "Failed to calculate reserve fee: " + feeRoe.error().message);
  }
  uint64_t reserveFee = feeRoe.value();
  const int64_t reserveAmount = static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY) - otherCosts -
                                static_cast<int64_t>(reserveFee);
  if (reserveAmount < 0) {
    return Error(2, "Initial token supply is insufficient for genesis fees and stakes");
  }
  return createSystemAccountRecord(
      config, key, AccountBuffer::ID_RESERVE,
      makeUserAccountFromKeys(key.reserve, reserveAmount, AccountAttachment::emptySerialized()), reserveFee,
      "reserve transaction");
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
    account.wallet.miner.registered = true;  // a new chain's committee: registered from block 0
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
                           const std::vector<GenesisMiner> &miners,
                           const Chain::ChainAuthority &authority) {
  // Roles of genesis block:
  // 1. Mark initial checkpoint with blockchain parameters
  // 2. Create the system accounts (fee, reserve, registrar, recycle)
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
  gm.authority = authority;
  // The genesis record carries the authority, so the admin keys that sign it sign the
  // generation-1 beacon certificate too.
  if (auto valid = chain_tx::validateGenesisAuthority(gm.authority, gm.genesis.wallet, config.networkId);
      !valid) {
    return Error(2, valid.error().message);
  }

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

  // System accounts. Fee comes first: the other records pay their fees into
  // it. Fee, registrar and recycle start empty; reserve takes the rest of the
  // supply (reserve funds the registrar later).
  struct ZeroAccount {
    uint64_t id;
    const AccountKeys *keys;
    const char *label;
    uint64_t fee{0};
    Ledger::Record rec;
  };
  ZeroAccount zeroAccounts[] = {{AccountBuffer::ID_FEE, &key.fee, "fee transaction", 0, {}},
                                {AccountBuffer::ID_REGISTRAR, &key.registrar, "registrar transaction", 0, {}},
                                {AccountBuffer::ID_RECYCLE, &key.recycle, "recycle transaction", 0, {}}};
  int64_t zeroAccountFees = 0;
  for (auto &zero : zeroAccounts) {
    auto recRoe = createSystemAccountRecord(
        config, key, zero.id, makeUserAccountFromKeys(*zero.keys, 0, AccountAttachment::emptySerialized()),
        zero.fee, zero.label);
    if (!recRoe) {
      return recRoe.error();
    }
    zero.rec = std::move(recRoe.value());
    zeroAccountFees += static_cast<int64_t>(zero.fee);
  }

  auto minerRecords = createGenesisMinerRecords(config, key, miners);
  if (!minerRecords) {
    return minerRecords.error();
  }
  const int64_t minerTotal = minerRecords.value().second;

  auto reserveRec = createReserveRecord(config, key, zeroAccountFees + minerTotal);
  if (!reserveRec) {
    return reserveRec.error();
  }

  genesisBlock.block.records.push_back(std::move(zeroAccounts[0].rec)); // fee
  genesisBlock.block.records.push_back(std::move(reserveRec.value()));
  genesisBlock.block.records.push_back(std::move(zeroAccounts[1].rec)); // registrar
  genesisBlock.block.records.push_back(std::move(zeroAccounts[2].rec)); // recycle
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

Beacon::Roe<Client::AuthorityProof> Beacon::authorityProof(uint64_t upToBlockId) const {
  auto proof = chain_.authorityProof(upToBlockId);
  if (!proof) {
    return Error(proof.error().code, proof.error().message);
  }
  return proof.value();
}


Beacon::Roe<void> Beacon::applyPendingRewind(const std::string &ledgerDir) {
  auto applied = rewind::applyPendingOrder(config_.workDir, ledgerDir);
  if (!applied) {
    return Error(3, "Failed to apply the pending rewind: " + applied.error().message);
  }
  if (const auto &order = applied.value()) {
    log().error << "Rewound: kept blocks up to " << order->keptUpTo << ", dropped " << order->keptUpTo + 1
                << " to " << order->droppedUpTo << " (past the cutover of beacon generation " << order->generation
                << ")";
    // A tip above the kept blocks was signed by the superseded generation, which this beacon never
    // signs with again (it stands down, or commits nothing above the cutover but a certificate).
    if (auto dropped = dropSignedTipAbove(order->keptUpTo); !dropped) {
      return dropped;
    }
  }
  auto last = rewind::lastApplied(config_.workDir);
  if (!last) {
    return Error(3, last.error().message);
  }
  lastRewind_ = last.value();
  return {};
}

Beacon::Roe<bool> Beacon::scheduleRewind(uint64_t approvedGeneration) {
  auto scheduled = rewind::schedule(chain_, config_.workDir, approvedGeneration, maxAutomaticRewind_);
  if (!scheduled) {
    return Error(3, "Failed to write the rewind order: " + scheduled.error().message);
  }
  const auto &order = scheduled.value().order;
  switch (scheduled.value().kind) {
  case rewind::Scheduled::Kind::None:
    return false;
  case rewind::Scheduled::Kind::HeldBack:
    if (heldBackGeneration_ != order->generation) {
      heldBackGeneration_ = order->generation;
      const auto &depth = scheduled.value().depthSeconds;
      log().error << "Not rewinding on its own: beacon generation " << order->generation << " orders a cut to block "
                  << order->keptUpTo << ", "
                  << (depth ? std::to_string(*depth / 3600) + " h" : std::string("an unknown time"))
                  << " of slots back — more than the " << maxAutomaticRewind_.count() / 3600
                  << " h a node rewinds unattended. Check the certificate; to apply it, restart with "
                  << "--approve-rewind " << order->generation;
    }
    return false;
  case rewind::Scheduled::Kind::Forked:
    if (heldBackGeneration_ != order->generation) {
      heldBackGeneration_ = order->generation;
      log().error << "Not rewinding: beacon generation " << order->generation << " vouches for another block "
                  << order->keptUpTo << " than this node holds — its history forked at or below it, or the "
                  << "certificate names the wrong block. Its tips past the cutover are refused; an operator "
                  << "must look";
    }
    return false;
  case rewind::Scheduled::Kind::Ordered:
    break;
  }
  log().error << "Rewinding to block " << order->keptUpTo << ": blocks " << order->keptUpTo + 1 << " to "
              << order->droppedUpTo << " are past the cutover of beacon generation " << order->generation
              << "; restarting";
  return true;
}


Beacon::Roe<void> Beacon::checkUnsignedLedger(const TipSigner &signer, bool acceptLedgerTip) const {
  const auto authority = chain_.getAuthority();
  const uint64_t nextBlockId = getNextBlockId();
  if (nextBlockId <= 1 || droppedTipThisStart_ || !authority || !signer.holdsKey(authority->beacon.publicKey)) {
    return {}; // only the genesis block, a rewind, or a key not in force (it signs nothing yet)
  }
  if (acceptLedgerTip) {
    log().warning << "No signed tip: signing the ledger's block " << nextBlockId - 1
                  << " as the tip (--accept-ledger-tip)";
    return {};
  }
  return Error(5, "No " + std::string(FILE_SIGNED_TIP) + ", but the ledger holds blocks up to " +
                      std::to_string(nextBlockId - 1) +
                      ": a beacon directory moved from another host must bring its signed tip (copy the whole "
                      "work directory after a clean stop). Signing this ledger's tip could sign a height the "
                      "last host signed with another block (an equivocation halts the network). If this "
                      "ledger certainly holds every block this key signed, start once with --accept-ledger-tip");
}

Beacon::Roe<void> Beacon::dropSignedTipAbove(uint64_t height) {
  const std::string path = config_.workDir + "/" + FILE_SIGNED_TIP;
  auto bytes = node_file::readIfExists(path);
  if (!bytes) {
    return Error(3, bytes.error().message);
  }
  Client::SignedTip tip;
  if (!bytes.value() || !tip.ltsFromString(*bytes.value()) || tip.height <= height) {
    return {}; // none, or unreadable (openTipSigning reports it), or still within the kept blocks
  }
  if (auto removed = node_file::remove(path); !removed) {
    return Error(3, removed.error().message);
  }
  droppedTipThisStart_ = true;
  log().warning << "Dropped the signed tip for block " << tip.height << " (rewound to block " << height << ")";
  return {};
}

} // namespace pp
