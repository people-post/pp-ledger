#include "server/Beacon.h"
#include "chain/ImportTxHandler.h"
#include "server/NodeFile.h"
#include "ledger/chain/AccountPolicy.h"
#include "ledger/chain/TxFees.h"
#include "ledger/client/AccountAttachment.h"
#include "ledger/client/Client.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Crypto.h"
#include "common/Logger.h"
#include "ledger/lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <set>

namespace pp {

namespace {


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

Beacon::Beacon() : NodeCore("Beacon") {}

bool Beacon::isInDeliveryWindow(uint64_t slot) const { return chain_.isInDeliveryWindow(slot); }

Beacon::Roe<void> Beacon::verifyMinerRegistration(const Client::MinerInfo &miner,
                                                 const std::string &networkId) const {
  auto signed_ = verifyAccountSignatures(miner.id, miner.signingMessage(networkId), miner.signatures);
  if (!signed_) {
    return Error(signed_.error().code, "Registration not signed by miner " + std::to_string(miner.id) + ": " +
                                           signed_.error().message);
  }
  // Only a miner the chain registered (T_MINER_REGISTER: stake and fee) is one: any other account's
  // registration would earn a peer the trusted lane.
  auto account = getAccount(miner.id);
  if (!account || !account.value().wallet.miner.registered) {
    return Error("Account " + std::to_string(miner.id) + " is not a registered miner");
  }
  return {};
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

  // A genesis signed offline names its config, genesis time included; else the network starts now.
  auto chainConfig = config.chain;
  chainConfig.genesisTime = utl::getCurrentTime();
  if (!config.signedGenesis.empty()) {
    auto gm = chain_genesis::metaOf(config.signedGenesis.front());
    if (!gm) {
      return Error(2, gm.error().message);
    }
    chainConfig = gm.value().config;
  }

  // Initialize consensus
  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = chainConfig.genesisTime;
  consensusConfig.timeOffsetMs = 0;
  consensusConfig.slotDuration = chainConfig.slotDuration;
  consensusConfig.slotsPerEpoch = chainConfig.slotsPerEpoch;
  chain_.initConsensus(consensusConfig);

  // Initialize ledger
  Ledger::InitConfig ledgerConfig;
  setWorkDir(config.workDir);
  ledgerConfig.workDir = ledgerDir();
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = chain_.initLedger(ledgerConfig);
  if (!ledgerResult) {
    return Error(2, "Failed to initialize ledger: " +
                        ledgerResult.error().message);
  }
  // The beacon signs what it commits: every block is durable before it is signed (B009).
  chain_.setLedgerCommitPolicy(Ledger::CommitPolicy::everyBlock());

  // Create and add genesis block
  auto authority = config.authority;
  authority.beacon.networkId = chainConfig.networkId;
  if (config.import.has_value() && !config.miners.empty()) {
    return Error(2, "A re-genesis imports its miners: give no genesis miners");
  }
  if (!config.signedGenesis.empty() && config.import.has_value()) {
    return Error(2, "A re-genesis is not signed offline yet: init it with the genesis keys");
  }
  auto genesisBlockResult = !config.signedGenesis.empty() ? createSignedGenesisBlock(config.signedGenesis)
                            : config.import.has_value()
                                ? createImportingGenesisBlock(chainConfig, config.key, authority, *config.import)
                                : createGenesisBlock(chainConfig, config.key, config.miners, authority);
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
  if (config.import.has_value()) {
    if (auto imported = addImportBlocks(*config.import, chainConfig.genesisTime, chainConfig.maxBlockBytes);
        !imported) {
      return imported;
    }
  }

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

  setWorkDir(config.workDir);
  const std::string ledgerPath = ledgerDir();
  if (auto rewound = applyPendingRewind(); !rewound) {
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

  if (auto opened = openStores(); !opened) {
    return opened;
  }

  log().info << "Beacon mounted successfully";
  log().info << "  Loaded " << blockCount << " blocks from ledger";
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
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
  refreshStores();
  return {};
}

Beacon::Roe<void> Beacon::openTipSigning(const std::vector<utl::MlDsaKeyPair> &keys, bool acceptLedgerTip) {
  auto signer = std::make_unique<TipSigner>(workDir() + "/" + FILE_SIGNED_TIP, keys);
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
  if (auto shape = chain_genesis::checkKeys(key); !shape) {
    return Error(2, shape.error().message);
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

Beacon::Roe<Ledger::ChainNode> Beacon::createImportingGenesisBlock(const Chain::BlockChainConfig &config,
                                                                  const InitKeyConfig &key,
                                                                  const Chain::ChainAuthority &authority,
                                                                  const StateSnapshot &snapshot) {
  log().info << "Creating an importing genesis block: " << snapshot.accounts.size() << " accounts of network '"
             << snapshot.predecessor.networkId << "' at block " << snapshot.predecessor.height;
  if (auto keysOk = checkInitKeys(key); !keysOk) {
    return keysOk.error();
  }
  Ledger::ChainNode genesisBlock;
  genesisBlock.block.index = 0;
  genesisBlock.block.timestamp = config.genesisTime;
  genesisBlock.block.previousHash = utl::zeroHash();

  Chain::GenesisAccountMeta gm;
  gm.config = config;
  gm.genesis = chain_genesis::accountOf(key.genesis, 0, AccountAttachment::emptySerialized());
  gm.genesis.wallet.mBalances.clear(); // they come with the import
  gm.authority = authority;
  if (auto valid = chain_tx::validateGenesisAuthority(gm.authority, gm.genesis.wallet, config.networkId); !valid) {
    return Error(2, valid.error().message);
  }
  gm.hasImport = true;
  gm.import.predecessor = snapshot.predecessor;
  gm.import.accountCount = snapshot.accounts.size();
  for (const auto &[id, keys] : {std::pair<uint64_t, const AccountKeys *>{AccountBuffer::ID_RESERVE, &key.reserve},
                                 {AccountBuffer::ID_REGISTRAR, &key.registrar},
                                 {AccountBuffer::ID_FEE, &key.fee},
                                 {AccountBuffer::ID_RECYCLE, &key.recycle}}) {
    const auto wallet = chain_genesis::accountOf(*keys, 0, "").wallet;
    gm.import.systemKeys.push_back(SystemAccountKeys{id, wallet.publicKeys, wallet.minSignatures});
  }

  Ledger::TxGenesis txGenesis;
  txGenesis.fee = 0;
  txGenesis.meta = gm.ltsToString();
  Ledger::Record rec;
  rec.type = Ledger::T_GENESIS;
  rec.data = utl::binaryPack(txGenesis);
  if (auto signedRec = signWithGenesisKeys(rec, key.genesisSigners, config.networkId, "checkpoint transaction");
      !signedRec) {
    return signedRec.error();
  }
  genesisBlock.block.records.push_back(rec);
  auto sealed = chain_.sealBlock(genesisBlock);
  if (!sealed) {
    return Error(2, "Failed to seal the importing genesis block: " + sealed.error().message);
  }
  return genesisBlock;
}

Beacon::Roe<void> Beacon::addImportBlocks(const StateSnapshot &snapshot, int64_t genesisTime, uint64_t maxBlockBytes) {
  // Room for the records: the block's header fields around them (a few hundred bytes).
  constexpr uint64_t kHeaderRoom = 4096;
  const uint64_t budget = maxBlockBytes > kHeaderRoom ? maxBlockBytes - kHeaderRoom : 0;
  std::vector<Ledger::Record> records;
  uint64_t bytes = 0;
  auto seal = [&]() -> Roe<void> {
    auto last = chain_.readLastBlock();
    if (!last) {
      return Error(2, "Failed to read the last block: " + last.error().message);
    }
    auto block = chain_.linkNextBlock(last.value(), 0, 0, genesisTime, std::move(records));
    records.clear();
    bytes = 0;
    if (auto sealed = chain_.sealBlock(block); !sealed) {
      return Error(2, "Failed to seal import block " + std::to_string(block.block.index) + ": " +
                          sealed.error().message);
    }
    return addBlock(block);
  };
  for (const auto &account : snapshot.accounts) {
    Ledger::Record record = ImportTxHandler::makeRecord(account);
    const uint64_t size = utl::binaryPack(record).size();
    if (size > budget) {
      return Error(2, "Account " + std::to_string(account.id) + " (" + std::to_string(size) +
                          " bytes) does not fit a block: raise maxBlockBytes");
    }
    if (bytes + size > budget) {
      if (auto added = seal(); !added) {
        return added;
      }
    }
    bytes += size;
    records.push_back(std::move(record));
  }
  if (!records.empty()) {
    if (auto added = seal(); !added) {
      return added;
    }
  }
  log().info << "Imported " << snapshot.accounts.size() << " accounts in blocks 1 to " << chain_.getNextBlockId() - 1;
  return {};
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
  chain_genesis::Plan plan;
  plan.config = config;
  plan.keys = key;
  plan.names = key.names;
  plan.miners = miners;
  plan.authority = authority;
  auto records = chain_genesis::records(plan);
  if (!records) {
    return Error(2, records.error().message);
  }
  for (auto &record : records.value()) {
    if (auto signedRec = signWithGenesisKeys(record, key.genesisSigners, config.networkId, "genesis record");
        !signedRec) {
      return signedRec.error();
    }
  }
  return sealGenesisBlock(std::move(records.value()), config.genesisTime);
}

Beacon::Roe<Ledger::ChainNode> Beacon::createSignedGenesisBlock(const std::vector<Ledger::Record> &records) {
  log().info << "Creating the genesis block signed offline (" << records.size() << " records)";
  if (records.empty()) {
    return Error(2, "The signed genesis has no records");
  }
  auto gm = chain_genesis::metaOf(records.front());
  if (!gm) {
    return Error(2, gm.error().message);
  }
  // Sealing applies every record and checks its signatures against the genesis account.
  return sealGenesisBlock(records, gm.value().config.genesisTime);
}

Beacon::Roe<Ledger::ChainNode> Beacon::sealGenesisBlock(std::vector<Ledger::Record> records, int64_t genesisTime) {
  Ledger::ChainNode genesisBlock;
  genesisBlock.block.index = 0;
  genesisBlock.block.timestamp = genesisTime;
  genesisBlock.block.previousHash = utl::zeroHash();
  genesisBlock.block.slot = 0;
  genesisBlock.block.slotLeader = 0;
  genesisBlock.block.epoch = 0;
  genesisBlock.block.records = std::move(records);
  auto sealResult = chain_.sealBlock(genesisBlock);
  if (!sealResult) {
    return Error(19, "Failed to seal genesis block: " + sealResult.error().message);
  }
  log().debug << "Genesis block created with hash: " << genesisBlock.hash;
  return genesisBlock;
}

Beacon::Roe<void> Beacon::onRewound(uint64_t keptUpTo) {
  // A tip above the kept blocks was signed by the superseded generation, which this beacon never
  // signs with again (it stands down, or commits nothing above the cutover but a certificate).
  return dropSignedTipAbove(keptUpTo);
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
  const std::string path = workDir() + "/" + FILE_SIGNED_TIP;
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
