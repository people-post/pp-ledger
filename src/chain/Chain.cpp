#include "Chain.h"
#include "BlockValidation.h"
#include "RecordHandler.h"
#include "TxFees.h"
#include "TxLedgerMeta.h"
#include "TxSignatures.h"
#include "common/Logger.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Serialize.hpp"
#include "lib/common/Utilities.h"
#include "../consensus/EpochSeed.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <type_traits>
#include <utility>

namespace pp {

namespace {

template <typename T> Chain::Roe<T> mapTx(chain_tx::Roe<T> r) {
  if (!r) {
    return Chain::Error(r.error());
  }
  return std::move(r.value());
}

Chain::Roe<void> mapTxVoid(chain_tx::Roe<void> r) {
  if (!r) {
    return Chain::Error(r.error());
  }
  return {};
}

} // namespace

Chain::Chain() {
  redirectLogger("Chain");
  txContext_.ledger.redirectLogger(log().getFullName() + ".Ledger");
  txContext_.consensus.redirectLogger(log().getFullName() + ".Obo");
  recordHandler_.redirectLoggers(log().getFullName());
  txContext_.fnAccountMetaForRecord = FnAccountMetaForRecord{
      [this](const Ledger::Record &rec, uint64_t accountId) {
        return recordHandler_.getUserAccountMeta(rec, accountId);
      },
      [this](const Ledger::Record &rec, const Ledger::Block &block) {
        return recordHandler_.getGenesisAccountMeta(rec, block);
      }};
  txContext_.fnIdempotencyKeyForRecord =
      [this](const Ledger::Record &rec) {
        return recordHandler_.getIdempotencyKey(rec);
      };
  txContext_.fnBillableCustomMetaSizeForFee =
      [this](const BlockChainConfig &config, const Ledger::TypedTx &tx) {
        return recordHandler_.getBillableCustomMetaSizeForFee(config, tx);
      };
}

bool Chain::isStakeholderSlotLeader(uint64_t stakeholderId,
                                    uint64_t slot) const {
  return txContext_.consensus.isSlotLeader(slot, stakeholderId);
}

bool Chain::isSlotBlockProductionTime(uint64_t slot) const {
  return txContext_.consensus.isSlotBlockProductionTime(slot);
}

bool Chain::isChainConfigReady() const {
  return txContext_.optChainConfig.has_value();
}

chain_block::BlockAdmissionMode
Chain::admissionModeFor(uint64_t blockIndex) const {
  // Live tip from genesis (or never mounted at a later checkpoint).
  if (txContext_.checkpoint.currentId == 0) {
    return chain_block::BlockAdmissionMode::Full;
  }
  // Late join: loadFromLedger(startingBlockId) sets lastId == currentId ==
  // startingBlockId. Until a later checkpoint rotates currentId forward,
  // ingest stays CheckpointReplay (structural + soft txs). Tip peers that
  // started from genesis keep Full for index >= currentId.
  if (txContext_.checkpoint.currentId == txContext_.checkpoint.lastId) {
    return chain_block::BlockAdmissionMode::CheckpointReplay;
  }
  return blockIndex >= txContext_.checkpoint.currentId
             ? chain_block::BlockAdmissionMode::Full
             : chain_block::BlockAdmissionMode::CheckpointReplay;
}

bool Chain::needsCheckpoint(const BlockChainConfig &config) const {
  const uint64_t age = chain_block::getBlockAgeSeconds(
      txContext_.checkpoint.currentId, txContext_.ledger,
      txContext_.consensus);
  return chain_block::needsCheckpoint(config, txContext_.checkpoint,
                                      getNextBlockId(), age);
}

Chain::Checkpoint Chain::getCheckpoint() const { return txContext_.checkpoint; }

uint64_t Chain::getNextBlockId() const {
  return txContext_.ledger.getNextBlockId();
}

int64_t Chain::getConsensusTimestamp() const {
  return txContext_.consensus.getTimestamp();
}

int64_t Chain::getSlotStartTime(uint64_t slot) const {
  return txContext_.consensus.getSlotStartTime(slot);
}

uint64_t Chain::getSlotDuration() const {
  return txContext_.optChainConfig.has_value()
             ? txContext_.optChainConfig.value().slotDuration
             : 0;
}

uint64_t Chain::getCurrentSlot() const {
  return txContext_.consensus.getCurrentSlot();
}

uint64_t Chain::getCurrentEpoch() const {
  return txContext_.consensus.getCurrentEpoch();
}

uint64_t Chain::getTotalStake() const {
  return txContext_.consensus.getTotalStake();
}

uint64_t Chain::getStakeholderStake(uint64_t stakeholderId) const {
  return txContext_.consensus.getStake(stakeholderId);
}

uint64_t Chain::getMaxTransactionsPerBlock() const {
  return txContext_.optChainConfig.has_value()
             ? txContext_.optChainConfig.value().maxTransactionsPerBlock
             : 0;
}

uint64_t Chain::getHeartbeatSlots() const {
  return txContext_.optChainConfig.has_value()
             ? txContext_.optChainConfig.value().heartbeatSlots
             : 0;
}

std::string Chain::getNetworkId() const {
  return txContext_.optChainConfig.has_value()
             ? txContext_.optChainConfig.value().networkId
             : std::string{};
}

Chain::Roe<uint64_t> Chain::getSlotLeader(uint64_t slot) const {
  auto result = txContext_.consensus.getSlotLeader(slot);
  if (!result) {
    return Error(E_CONSENSUS_QUERY,
                 "Failed to get slot leader: " + result.error().message);
  }
  return result.value();
}

std::vector<consensus::Stakeholder> Chain::getStakeholders() const {
  return txContext_.consensus.getStakeholders();
}

Chain::Roe<Ledger::ChainNode> Chain::readBlock(uint64_t blockId) const {
  auto result = txContext_.ledger.readBlock(blockId);
  if (!result) {
    return Error(E_BLOCK_NOT_FOUND,
                 "Block not found: " + std::to_string(blockId));
  }
  return result.value();
}

Chain::Roe<Client::UserAccount> Chain::getAccount(uint64_t accountId) const {
  auto roeAccount = txContext_.bank.getAccount(accountId);
  if (!roeAccount) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "Account not found: " + std::to_string(accountId));
  }
  auto const &account = roeAccount.value();
  Client::UserAccount userAccount;
  userAccount.wallet = account.wallet;

  // Attachment is not kept in AccountBuffer (hot path stays light). Hydrate
  // ad-hoc from the account tip block.
  const auto &fns = txContext_.fnAccountMetaForRecord;
  if (!fns.has_value()) {
    return Error(E_INTERNAL,
                 "Account meta extractors not configured on TxContext");
  }
  auto blockResult = txContext_.ledger.readBlock(account.blockId);
  if (!blockResult) {
    return Error(E_BLOCK_NOT_FOUND,
                 "Block not found: " + std::to_string(account.blockId));
  }
  auto metaRoe = chain_tx::getAccountCustomMetaFromBlock(
      blockResult.value().block, accountId, fns->fnUser, fns->fnGenesis);
  if (!metaRoe) {
    return Error(metaRoe.error());
  }
  userAccount.meta = metaRoe.value();
  return userAccount;
}

Chain::Roe<std::string> Chain::getUpdatedAccountMetadataForRenewal(
    const Ledger::Block &block, const AccountBuffer::Account &account,
    uint64_t minFee) const {
  const auto &fns = txContext_.fnAccountMetaForRecord;
  if (!fns.has_value()) {
    return Error(E_INTERNAL,
                 "Account meta extractors not configured on TxContext");
  }
  return mapTx(chain_tx::getUpdatedAccountMetadataForRenewal(
      block, account, minFee, fns->fnUser, fns->fnGenesis));
}

Chain::Roe<Ledger::Record>
Chain::createRenewalTx(uint64_t accountId) const {
  auto accountResult = txContext_.bank.getAccount(accountId);
  if (!accountResult) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "Account not found: " + std::to_string(accountId));
  }

  auto const &account = accountResult.value();
  Ledger::TxRenewal tx;
  tx.walletId = accountId;
  uint16_t type = Ledger::T_RENEWAL;

  // Compute minimum fee from current account metadata state.
  auto minimumFeeResult =
      calculateMinimumFeeForAccountMeta(txContext_.bank, accountId);
  if (!minimumFeeResult) {
    return minimumFeeResult.error();
  }
  const uint64_t minimumFee = minimumFeeResult.value();

  if (accountId != AccountBuffer::ID_GENESIS &&
      accountId != AccountBuffer::ID_FEE) {
    auto balance =
        txContext_.bank.getBalance(accountId, AccountBuffer::ID_GENESIS);
    if (balance < static_cast<int64_t>(minimumFee)) {
      // Insufficient balance for renewal, terminate account with whatever
      // balance remains. Fee is 0 here; all remaining balances are transferred
      // to recycle account. Never terminate fee account (it pays fee to self).
      type = Ledger::T_END_USER;
      tx.fee = 0;
    }
  }

  if (type == Ledger::T_RENEWAL) {
    // Get account metadata from previous block
    auto blockResult = txContext_.ledger.readBlock(account.blockId);
    if (!blockResult) {
      return Error(E_BLOCK_NOT_FOUND,
                   "Block not found: " + std::to_string(account.blockId));
    }
    auto const &block = blockResult.value().block;
    tx.fee = minimumFee;
    auto metaResult =
        getUpdatedAccountMetadataForRenewal(block, account, minimumFee);
    if (!metaResult) {
      return metaResult.error();
    }
    tx.meta = metaResult.value();
  }
  // T_END_USER does not need metadata update

  Ledger::Record rec;
  rec.type = type;
  if (type == Ledger::T_END_USER) {
    Ledger::TxEndUser endTx;
    endTx.walletId = tx.walletId;
    endTx.fee = tx.fee;
    endTx.meta = tx.meta;
    rec.data = utl::binaryPack(endTx);
  } else {
    rec.data = utl::binaryPack(tx);
  }
  rec.signatures = {};
  return rec;
}

Chain::Roe<uint64_t>
Chain::calculateMaxBlockIdForRenewal(uint64_t atBlockId) const {
  return mapTx(chain_block::calculateMaxBlockIdForRenewal(
      txContext_.ledger, txContext_.consensus, txContext_.optChainConfig,
      txContext_.checkpoint, atBlockId));
}

Chain::Roe<std::vector<Ledger::Record>>
Chain::collectRenewals(uint64_t /*slot*/) const {
  std::vector<Ledger::Record> renewals;
  const uint64_t nextBlockId = txContext_.ledger.getNextBlockId();

  auto maxBlockIdResult = calculateMaxBlockIdForRenewal(nextBlockId);
  if (!maxBlockIdResult) {
    return maxBlockIdResult.error();
  }
  const uint64_t maxBlockIdForRenewal = maxBlockIdResult.value();
  if (maxBlockIdForRenewal == 0) {
    return renewals;
  }

  for (uint64_t accountId :
       txContext_.bank.getAccountIdsBeforeBlockId(maxBlockIdForRenewal)) {
    auto renewalResult = createRenewalTx(accountId);
    if (!renewalResult) {
      return renewalResult.error();
    }
    renewals.push_back(renewalResult.value());
  }

  return renewals;
}

Chain::Roe<Ledger::ChainNode> Chain::readLastBlock() const {
  auto result = txContext_.ledger.readLastBlock();
  if (!result) {
    return Error(E_LEDGER_READ,
                 "Failed to read last block: " + result.error().message);
  }
  return result.value();
}

Chain::Roe<uint64_t>
Chain::calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                         const Ledger::TypedTx &tx) const {
  return mapTx(chain_tx::calculateMinimumFeeForTransaction(
      config, tx,
      [this](const BlockChainConfig &cfg, const Ledger::TypedTx &t) {
        return recordHandler_.getBillableCustomMetaSizeForFee(cfg, t);
      }));
}

Chain::Roe<std::vector<Ledger::Record>>
Chain::findTransactionsByWalletId(uint64_t walletId,
                                  uint64_t &ioBlockId) const {
  // ioBlockId is the block ID to start scanning from (exclusive). It is updated
  // to the last scanned block ID. Client sends 0 to mean "latest" (scan from
  // tip); we substitute getNextBlockId() so that scanning runs.

  std::vector<Ledger::Record> out;
  uint64_t nextId = txContext_.ledger.getNextBlockId();
  if (ioBlockId == 0) {
    ioBlockId = nextId;
  }
  ioBlockId = std::min(ioBlockId, nextId);
  if (ioBlockId == 0) {
    return out;
  }

  size_t nBlocksScanned = 0;
  uint64_t currentBlockId = ioBlockId;
  while (currentBlockId > 0 &&
         nBlocksScanned < MAX_BLOCKS_TO_SCAN_FOR_WALLET_TX) {
    --currentBlockId;
    auto blockRoe = txContext_.ledger.readBlock(currentBlockId);
    if (!blockRoe) {
      return Error(E_BLOCK_NOT_FOUND,
                   "Block not found: " + std::to_string(currentBlockId));
    }
    auto const &recs = blockRoe.value().block.records;
    for (auto it = recs.rbegin(); it != recs.rend(); ++it) {
      if (recordHandler_.matchesWalletForIndex(*it, walletId)) {
        out.push_back(*it);
      }
    }
    ++nBlocksScanned;
    if (out.size() >= THRESHOLD_TXES_FOR_WALLET_TX) {
      break;
    }
  }
  ioBlockId = currentBlockId;
  return out;
}

Chain::Roe<Ledger::Record>
Chain::findTransactionByIndex(uint64_t txIndex) const {
  const uint64_t firstBlockId = txContext_.ledger.getStartingBlockId();
  const uint64_t nextBlockId = txContext_.ledger.getNextBlockId();
  if (nextBlockId <= firstBlockId) {
    return Error(E_LEDGER_READ, "No blocks in ledger");
  }

  auto lastBlockRoe = txContext_.ledger.readLastBlock();
  if (!lastBlockRoe) {
    return Error(E_LEDGER_READ,
                 "Failed to read last block: " + lastBlockRoe.error().message);
  }

  const auto &lastBlock = lastBlockRoe.value().block;
  const uint64_t lastBlockTxCount =
      static_cast<uint64_t>(lastBlock.records.size());
  const uint64_t totalTxCount = lastBlock.txIndex + lastBlockTxCount;

  if (txIndex >= totalTxCount) {
    return Error(E_INVALID_ARGUMENT,
                 "Transaction index out of range: " + std::to_string(txIndex) +
                     " >= " + std::to_string(totalTxCount));
  }

  uint64_t low = firstBlockId;
  uint64_t high = nextBlockId - 1;

  while (low <= high) {
    const uint64_t mid = low + (high - low) / 2;
    auto blockRoe = txContext_.ledger.readBlock(mid);
    if (!blockRoe) {
      return Error(
          E_LEDGER_READ,
          "Failed to read block " + std::to_string(mid) +
              " during findTransactionByIndex: " + blockRoe.error().message);
    }

    const auto &block = blockRoe.value().block;
    const uint64_t blockStart = block.txIndex;
    const uint64_t blockTxCount =
        static_cast<uint64_t>(block.records.size());

    if (txIndex < blockStart) {
      if (mid == firstBlockId) {
        break;
      }
      high = mid - 1;
      continue;
    }

    if (blockTxCount == 0) {
      low = mid + 1;
      continue;
    }

    const uint64_t blockEnd = blockStart + blockTxCount; // exclusive
    if (txIndex >= blockEnd) {
      low = mid + 1;
      continue;
    }

    const uint64_t localIndex = txIndex - blockStart;
    return block.records[static_cast<size_t>(localIndex)];
  }

  return Error(E_LEDGER_READ, "Transaction index " + std::to_string(txIndex) +
                                  " not found in any block");
}

std::string Chain::calculateHash(const Ledger::Block &block) const {
  return chain_block::calculateBlockHash(block);
}

Chain::Roe<void> Chain::assembleBlockHeader(Ledger::ChainNode &block) {
  if (block.block.index == 0) {
    block.block.epoch = 0;
    block.block.stakeSnapshotHash =
        chain_block::calculateStakeSnapshotHash({});
    if (block.block.records.empty() ||
        block.block.records[0].type != Ledger::T_GENESIS) {
      return Error(E_BLOCK_VALIDATION,
                   "Genesis seal requires T_GENESIS as first record");
    }
    auto genesisTx =
        utl::binaryUnpack<Ledger::TxGenesis>(block.block.records[0].data);
    if (!genesisTx) {
      return Error(E_BLOCK_VALIDATION, "Failed to unpack genesis tx for epochSeed");
    }
    GenesisAccountMeta gm;
    if (!gm.ltsFromString(genesisTx->meta)) {
      return Error(E_BLOCK_VALIDATION, "Failed to unpack genesis meta for epochSeed");
    }
    const std::string cfgDigest =
        chain_block::calculateGenesisConfigDigest(gm.config);
    block.block.epochSeed =
        consensus::deriveGenesisEpochSeed(gm.config.networkId, cfgDigest);
    // Do not setEpochSeed yet — processGenesisTxRecord → GenesisTxHandler
    // calls consensus.init() and would clear it.
  } else {
    block.block.epoch =
        txContext_.consensus.getEpochFromSlot(block.block.slot);
    refreshStakeholders(block.block.slot);
    block.block.stakeSnapshotHash = chain_block::calculateStakeSnapshotHash(
        txContext_.consensus.getStakeholders());
    auto seedRoe = ensureEpochSeed(block.block.epoch);
    if (!seedRoe) {
      return seedRoe;
    }
    block.block.epochSeed = txContext_.consensus.getEpochSeed();
  }
  block.block.txRoot = chain_block::calculateTxRoot(block.block.records);
  return {};
}

Chain::Roe<void> Chain::sealBlock(Ledger::ChainNode &block) {
  if (pendingSeal_.has_value()) {
    return Error(E_BLOCK_VALIDATION,
                 "Cannot seal while a previous sealed block is uncommitted");
  }

  // Assemble → Check (non-genesis Full layers) → Apply (always Full) →
  // Commit-hold (pendingSeal). Do not use admissionModeFor for seal apply:
  // late-join CheckpointReplay is for trusted catch-up ingest only.
  auto assembled = assembleBlockHeader(block);
  if (!assembled) {
    return assembled;
  }

  if (block.block.index > 0) {
    if (admissionModeFor(block.block.index) !=
        chain_block::BlockAdmissionMode::Full) {
      return Error(E_BLOCK_VALIDATION,
                   "Cannot seal under CheckpointReplay; finish tip catch-up "
                   "first");
    }
    // Shared Full policy before apply (peers run the same layers via checkBlock).
    // Structural (hash) waits until stateRoot is known after apply.
    auto consensusCheck = mapTxVoid(
        chain_block::checkBlockConsensus(block, txContext_.consensus));
    if (!consensusCheck) {
      return Error(E_BLOCK_VALIDATION, consensusCheck.error().message);
    }
    auto bodyCheck = mapTxVoid(chain_block::checkBlockBodyPolicy(
        block, txContext_.bank, txContext_.ledger, txContext_.consensus,
        txContext_.optChainConfig, txContext_.checkpoint, recordHandler_));
    if (!bodyCheck) {
      return Error(E_BLOCK_VALIDATION, bodyCheck.error().message);
    }
  }

  // Single apply on the tip bank (same path addBlock would use). Matching
  // addBlock persists only — no second apply, no AccountBuffer overlay.
  for (const auto &rec : block.block.records) {
    Roe<void> applied;
    if (block.block.index == 0) {
      applied = processGenesisTxRecord(rec);
    } else {
      applied = processNormalTxRecord(
          rec, block.block.index, block.block.slot, block.block.slotLeader,
          chain_block::BlockAdmissionMode::Full);
    }
    if (!applied) {
      return Error(E_TX_VALIDATION,
                   "Failed to apply block for seal: " + applied.error().message);
    }
  }

  if (block.block.index == 0) {
    // Install after genesis apply (init clears any earlier seed).
    txContext_.consensus.setEpochSeed(0, block.block.epochSeed);
  }

  block.block.stateRoot = txContext_.bank.calculateStateRoot();
  block.hash = calculateHash(block.block);
  pendingSeal_ = PendingSeal{block.block.index, block.hash};
  return {};
}

void Chain::refreshStakeholders() {
  if (txContext_.consensus.isStakeUpdateNeeded()) {
    auto stakeholders = txContext_.bank.getStakeholders();
    txContext_.consensus.setStakeholders(stakeholders);
  }
  // Prefer tip epoch (chain progress) over wall-clock epoch so tests and
  // late-start nodes with mismatched genesisTime still keep a usable seed.
  uint64_t tipEpoch = 0;
  const uint64_t nextId = txContext_.ledger.getNextBlockId();
  if (nextId > 0) {
    auto tip = txContext_.ledger.readBlock(nextId - 1);
    if (tip) {
      tipEpoch = tip->block.epoch;
    }
  }
  if (!txContext_.consensus.hasEpochSeedFor(tipEpoch)) {
    (void)ensureEpochSeed(tipEpoch);
  }
  const uint64_t clockEpoch = txContext_.consensus.getCurrentEpoch();
  if (clockEpoch != tipEpoch &&
      !txContext_.consensus.hasEpochSeedFor(clockEpoch)) {
    (void)ensureEpochSeed(clockEpoch);
  }
}

void Chain::refreshStakeholders(uint64_t blockSlot) {
  uint64_t epoch = txContext_.consensus.getEpochFromSlot(blockSlot);
  if (txContext_.consensus.isStakeUpdateNeeded(epoch)) {
    auto stakeholders = txContext_.bank.getStakeholders();
    txContext_.consensus.setStakeholders(stakeholders, epoch);
  }
}

std::vector<std::string>
Chain::collectPrevEpochLookbackHashes(uint64_t prevEpoch) const {
  std::vector<std::string> newestFirst;
  const uint64_t nextId = txContext_.ledger.getNextBlockId();
  if (nextId == 0) {
    return {};
  }
  for (uint64_t id = nextId; id > 0; --id) {
    auto blockRoe = txContext_.ledger.readBlock(id - 1);
    if (!blockRoe) {
      break;
    }
    if (blockRoe->block.epoch > prevEpoch) {
      continue;
    }
    if (blockRoe->block.epoch < prevEpoch) {
      break;
    }
    newestFirst.push_back(blockRoe->hash);
    if (newestFirst.size() >= consensus::kEpochSeedLookback) {
      break;
    }
  }
  std::reverse(newestFirst.begin(), newestFirst.end());
  return newestFirst;
}

Chain::Roe<std::string> Chain::readPrevEpochSeed(uint64_t prevEpoch) const {
  const uint64_t nextId = txContext_.ledger.getNextBlockId();
  for (uint64_t id = nextId; id > 0; --id) {
    auto blockRoe = txContext_.ledger.readBlock(id - 1);
    if (!blockRoe) {
      break;
    }
    if (blockRoe->block.epoch == prevEpoch &&
        blockRoe->block.epochSeed.size() == utl::SHA256_DIGEST_SIZE) {
      return blockRoe->block.epochSeed;
    }
    if (blockRoe->block.epoch < prevEpoch) {
      break;
    }
  }
  return Error(E_STATE_INIT,
               "No epochSeed found for previous epoch " +
                   std::to_string(prevEpoch));
}

Chain::Roe<void> Chain::ensureEpochSeed(uint64_t epoch) {
  if (txContext_.consensus.hasEpochSeedFor(epoch)) {
    return {};
  }

  const std::string networkId =
      txContext_.optChainConfig.has_value()
          ? txContext_.optChainConfig->networkId
          : std::string{};

  if (epoch == 0) {
    if (!txContext_.optChainConfig.has_value()) {
      return Error(E_STATE_INIT,
                   "Cannot derive genesis epoch seed without chain config");
    }
    const std::string cfgDigest =
        chain_block::calculateGenesisConfigDigest(*txContext_.optChainConfig);
    const std::string seed =
        consensus::deriveGenesisEpochSeed(networkId, cfgDigest);
    txContext_.consensus.setEpochSeed(0, seed);
    return {};
  }

  auto prevSeedRoe = readPrevEpochSeed(epoch - 1);
  if (!prevSeedRoe) {
    return Error(prevSeedRoe.error().code, prevSeedRoe.error().message);
  }
  const std::string &prevSeed = prevSeedRoe.value();
  auto lookback = collectPrevEpochLookbackHashes(epoch - 1);
  const std::string tipMaterial = consensus::lookbackTipMaterial(
      epoch - 1, prevSeed, lookback);
  const std::string stakeHash = chain_block::calculateStakeSnapshotHash(
      txContext_.consensus.getStakeholders());
  const std::string seed = consensus::deriveEpochSeed(
      epoch, networkId, prevSeed, tipMaterial, stakeHash);
  txContext_.consensus.setEpochSeed(epoch, seed);
  return {};
}

void Chain::initConsensus(const consensus::SlotCommittee::Config &config) {
  txContext_.consensus.init(config);
}

void Chain::setClockOverride(std::optional<int64_t> unixSeconds) {
  txContext_.consensus.setClockOverride(unixSeconds);
}

void Chain::forceSlotLeader(uint64_t slot, uint64_t stakeholderId) {
  txContext_.consensus.forceSlotLeader(slot, stakeholderId);
}

void Chain::clearForcedSlotLeaders() {
  txContext_.consensus.clearForcedSlotLeaders();
}

Chain::Roe<void> Chain::initLedger(const Ledger::InitConfig &config) {
  auto result = txContext_.ledger.init(config);
  if (!result) {
    return Error(E_STATE_INIT,
                 "Failed to initialize ledger: " + result.error().message);
  }
  return {};
}

Chain::Roe<void> Chain::mountLedger(const std::string &workDir) {
  auto result = txContext_.ledger.mount(workDir);
  if (!result) {
    return Error(E_STATE_MOUNT,
                 "Failed to mount ledger: " + result.error().message);
  }
  return {};
}

Chain::Roe<uint64_t> Chain::loadFromLedger(uint64_t startingBlockId) {
  log().info << "Loading from ledger starting at block ID " << startingBlockId;

  log().info << "Resetting account buffer";
  txContext_.bank.reset();
  pendingSeal_.reset();

  // Process blocks from ledger one by one (replay existing chain state)
  // Starting block id is always a checkpoint id
  txContext_.checkpoint.lastId = startingBlockId;
  txContext_.checkpoint.currentId = startingBlockId;
  uint64_t blockId = startingBlockId;
  uint64_t logInterval = 1000; // Log every 1000 blocks
  // Same mode matrix as live late join: genesis→tip is Full; mount at a
  // checkpoint is CheckpointReplay for the whole replay loop (see
  // admissionModeFor / BLOCK_PIPELINE.md).
  const auto replayMode =
      startingBlockId == 0 ? chain_block::BlockAdmissionMode::Full
                           : chain_block::BlockAdmissionMode::CheckpointReplay;
  while (true) {
    auto blockResult = txContext_.ledger.readBlock(blockId);
    if (!blockResult) {
      // No more blocks to read
      break;
    }

    auto const &block = blockResult.value();
    if (blockId != block.block.index) {
      return Error(E_BLOCK_INDEX, "Block index mismatch: expected " +
                                      std::to_string(blockId) + " got " +
                                      std::to_string(block.block.index));
    }

    // Refresh stakeholders per epoch (so slot leader validation uses correct
    // stake for this block's epoch; no-op when still in same epoch).
    // Skip block 0 because:
    //   1. Genesis installs consensus during T_GENESIS apply.
    //   2. Consensus parameters are initialized while processing the genesis
    //   transaction.
    if (blockId > 0) {
      refreshStakeholders(block.block.slot);
      auto seedRoe = ensureEpochSeed(block.block.epoch);
      if (!seedRoe) {
        return Error(E_BLOCK_VALIDATION,
                     "Failed to ensure epoch seed for block " +
                         std::to_string(blockId) + ": " +
                         seedRoe.error().message);
      }
    }

    auto processResult = processBlock(block, replayMode);
    if (!processResult) {
      return Error(E_BLOCK_VALIDATION, "Failed to process block " +
                                           std::to_string(blockId) + ": " +
                                           processResult.error().message);
    }
    if (block.block.epochSeed.size() == utl::SHA256_DIGEST_SIZE) {
      txContext_.consensus.setEpochSeed(block.block.epoch,
                                        block.block.epochSeed);
    }

    blockId++;

    // Periodic progress logging
    if (blockId % logInterval == 0) {
      log().info << "Processed " << blockId << " blocks...";
    }
  }

  log().info << "Loaded " << blockId << " blocks from ledger";
  return blockId;
}

Chain::Roe<void> Chain::addBlock(const Ledger::ChainNode &block) {
  if (pendingSeal_.has_value()) {
    if (pendingSeal_->index != block.block.index ||
        pendingSeal_->hash != block.hash) {
      return Error(E_BLOCK_VALIDATION,
                   "Tip has an uncommitted sealed block; commit it before "
                   "adding a different block");
    }
    return commitSealedBlock(block);
  }

  const auto admissionMode = admissionModeFor(block.block.index);
  if (block.block.index > 0) {
    refreshStakeholders(block.block.slot);
    auto seedRoe = ensureEpochSeed(block.block.epoch);
    if (!seedRoe) {
      return Error(E_BLOCK_VALIDATION,
                   "Failed to ensure epoch seed: " + seedRoe.error().message);
    }
  }
  auto processResult = processBlock(block, admissionMode);
  if (!processResult) {
    return Error(E_BLOCK_VALIDATION,
                 "Failed to process block: " + processResult.error().message);
  }
  if (block.block.epochSeed.size() == utl::SHA256_DIGEST_SIZE) {
    txContext_.consensus.setEpochSeed(block.block.epoch, block.block.epochSeed);
  }

  auto ledgerResult = txContext_.ledger.addBlock(block);
  if (!ledgerResult) {
    return Error(E_LEDGER_WRITE,
                 "Failed to persist block: " + ledgerResult.error().message);
  }

  log().info << "Block added: " << block.block.index
             << " from slot leader: " << block.block.slotLeader;

  return {};
}

Chain::Roe<void> Chain::commitSealedBlock(const Ledger::ChainNode &block) {
  // Effects already applied in sealBlock; verify commitments then persist.
  if (block.block.index == 0) {
    auto genesisValidation =
        mapTxVoid(chain_block::validateGenesisBlock(block, recordHandler_));
    if (!genesisValidation) {
      return Error(E_BLOCK_VALIDATION, genesisValidation.error().message);
    }
  } else {
    auto structural = mapTxVoid(chain_block::checkBlock(
        block, chain_block::BlockAdmissionMode::SealedCommitVerify,
        txContext_.ledger, txContext_.consensus, txContext_.bank,
        txContext_.optChainConfig, txContext_.checkpoint, recordHandler_));
    if (!structural) {
      return Error(E_BLOCK_VALIDATION, structural.error().message);
    }
  }

  if (block.block.stateRoot != txContext_.bank.calculateStateRoot()) {
    return Error(E_BLOCK_HASH, "Block stateRoot mismatch");
  }

  // Rotate before persist so needsCheckpoint sees the same nextBlockId as the
  // normal processNormalBlock → addBlock path.
  maybeRotateCheckpoint(block);

  auto ledgerResult = txContext_.ledger.addBlock(block);
  if (!ledgerResult) {
    return Error(E_LEDGER_WRITE,
                 "Failed to persist block: " + ledgerResult.error().message);
  }

  if (block.block.epochSeed.size() == utl::SHA256_DIGEST_SIZE) {
    txContext_.consensus.setEpochSeed(block.block.epoch, block.block.epochSeed);
  }

  pendingSeal_.reset();

  log().info << "Block added: " << block.block.index
             << " from slot leader: " << block.block.slotLeader;

  return {};
}

void Chain::maybeRotateCheckpoint(const Ledger::ChainNode &block) {
  if (block.block.index == 0) {
    return;
  }
  if (txContext_.optChainConfig.has_value() &&
      needsCheckpoint(txContext_.optChainConfig.value()) &&
      block.block.index > txContext_.checkpoint.currentId) {
    txContext_.checkpoint.lastId = txContext_.checkpoint.currentId;
    txContext_.checkpoint.currentId = block.block.index;
    log().info << "Checkpoint rotated: last=" << txContext_.checkpoint.lastId
               << ", current=" << txContext_.checkpoint.currentId;
  }
}

Chain::Roe<void> Chain::processBlock(const Ledger::ChainNode &block,
                                     chain_block::BlockAdmissionMode mode) {
  if (block.block.index == 0) {
    return processGenesisBlock(block);
  } else {
    return processNormalBlock(block, mode);
  }
}

Chain::Roe<void> Chain::processGenesisBlock(const Ledger::ChainNode &block) {
  auto roe = mapTxVoid(chain_block::validateGenesisBlock(block, recordHandler_));
  if (!roe) {
    return Error(E_BLOCK_VALIDATION, "Block validation failed for block " +
                                         std::to_string(block.block.index) +
                                         ": " + roe.error().message);
  }

  for (const auto &rec : block.block.records) {
    auto result = processGenesisTxRecord(rec);
    if (!result) {
      return Error(E_TX_VALIDATION,
                   "Failed to process transaction: " + result.error().message);
    }
  }

  const std::string expectedStateRoot = txContext_.bank.calculateStateRoot();
  if (block.block.stateRoot != expectedStateRoot) {
    return Error(E_BLOCK_HASH, "Genesis block stateRoot mismatch");
  }

  return {};
}

Chain::Roe<void> Chain::processNormalBlock(
    const Ledger::ChainNode &block, chain_block::BlockAdmissionMode mode) {
  auto roe = mapTxVoid(chain_block::checkBlock(
      block, mode, txContext_.ledger, txContext_.consensus, txContext_.bank,
      txContext_.optChainConfig, txContext_.checkpoint, recordHandler_));
  if (!roe) {
    return Error(E_BLOCK_VALIDATION, "Block validation failed for block " +
                                         std::to_string(block.block.index) +
                                         ": " + roe.error().message);
  }

  for (const auto &rec : block.block.records) {
    auto result = processNormalTxRecord(rec, block.block.index, block.block.slot,
                                        block.block.slotLeader, mode);
    if (!result) {
      return Error(E_TX_VALIDATION,
                   "Failed to process transaction: " + result.error().message);
    }
  }

  const std::string expectedStateRoot = txContext_.bank.calculateStateRoot();
  if (block.block.stateRoot != expectedStateRoot) {
    return Error(E_BLOCK_HASH, "Block stateRoot mismatch");
  }

  maybeRotateCheckpoint(block);

  return {};
}

Chain::Roe<void> Chain::addBufferTransaction(
    AccountBuffer &bank,
    const Ledger::Record &record,
    uint64_t slotLeaderId) const {
  auto roe = validateTxSignatures(record, slotLeaderId,
                                  chain_block::BlockAdmissionMode::Full);
  if (!roe) {
    return Error(E_TX_SIGNATURE, "Failed to validate buffer transaction: " +
                                     roe.error().message);
  }

  auto blockId = getNextBlockId();
  const uint64_t currentSlot = getCurrentSlot();
  BufferApplyContext ctx{ txContext_,
                          blockId,
                          currentSlot,
                          chain_block::BlockAdmissionMode::Full };
  return mapTxVoid(recordHandler_.applyBuffer(record, bank, ctx));
}

Chain::Roe<void> Chain::processGenesisTxRecord(
    const Ledger::Record &record) {
  auto roe =
      validateTxSignatures(record, 0, chain_block::BlockAdmissionMode::Full);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  // Genesis records are applied as if they are in the genesis block (blockId=0).
  // Slot leader is not applicable for genesis init.
  BlockApplyContext ctx{ txContext_, 0, 0, 0,
                         chain_block::BlockAdmissionMode::Full };
  return mapTxVoid(recordHandler_.applyBlock(record, txContext_.bank, ctx));
}

Chain::Roe<void> Chain::processNormalTxRecord(
    const Ledger::Record &record, uint64_t blockId,
    uint64_t blockSlot, uint64_t slotLeaderId,
    chain_block::BlockAdmissionMode admissionMode) {
  auto roe = validateTxSignatures(record, slotLeaderId, admissionMode);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  BlockApplyContext ctx{ txContext_,
                         blockId,
                         blockSlot,
                         slotLeaderId,
                         admissionMode };
  return mapTxVoid(recordHandler_.applyBlock(record, txContext_.bank, ctx));
}

Chain::Roe<void> Chain::verifySignaturesAgainstAccount(
    const std::string &message, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account) const {
  return mapTxVoid(chain_tx::verifySignaturesAgainstAccount(
      message, signatures, account, txContext_.crypto, log()));
}

Chain::Roe<void> Chain::validateTxSignatures(
    const Ledger::Record &record,
    uint64_t slotLeaderId,
    chain_block::BlockAdmissionMode admissionMode) const {
  if (record.signatures.size() < 1) {
    return Error(E_TX_SIGNATURE,
                 "Transaction must have at least one signature");
  }

  auto signerAccountIdRoe =
      recordHandler_.getSignerAccountId(record, slotLeaderId);
  if (!signerAccountIdRoe) {
    return Error(signerAccountIdRoe.error());
  }
  const uint64_t signerAccountId = signerAccountIdRoe.value();

  auto accountResult = txContext_.bank.getAccount(signerAccountId);
  if (!accountResult) {
    if (chain_block::admissionTxStrict(admissionMode)) {
      if (txContext_.bank.isEmpty() &&
          signerAccountId == AccountBuffer::ID_GENESIS) {
        // Genesis account is created by the system checkpoint, this is not very
        // good way of handling Should avoid using this generic handlers for
        // specific case
        return {};
      }
      return Error(
          E_ACCOUNT_NOT_FOUND,
          "Failed to get account when validating transaction signatures: " +
              accountResult.error().message);
    } else {
      // CheckpointReplay: account may not exist yet in the catch-up bank.
      // Soft skip is only safe when the block source is trusted (beacon).
      return {};
    }
  }
  return verifySignaturesAgainstAccount(
      record.signingMessage(getNetworkId()), record.signatures,
      accountResult.value());
}

Chain::Roe<uint64_t>
Chain::calculateMinimumFeeForAccountMeta(const AccountBuffer &bank,
                                         uint64_t accountId) const {
  if (!txContext_.optChainConfig.has_value()) {
    return Error(E_INTERNAL,
                 "Chain config required for minimum fee from account meta");
  }
  const auto &fns = txContext_.fnAccountMetaForRecord;
  if (!fns.has_value()) {
    return Error(E_INTERNAL,
                 "Account meta extractors not configured on TxContext");
  }
  return mapTx(chain_tx::calculateMinimumFeeForAccountMeta(
      txContext_.ledger, txContext_.optChainConfig.value(), bank, accountId,
      fns->fnUser, fns->fnGenesis));
}

} // namespace pp
