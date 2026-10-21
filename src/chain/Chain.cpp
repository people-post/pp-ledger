#include "chain/Chain.h"
#include "ledger/chain/Domains.h"
#include "ledger/chain/AccountPolicy.h"
#include "chain/BeaconCertTxHandler.h"
#include "chain/BlockValidation.h"
#include "ledger/chain/ConfigPolicy.h"
#include "chain/EndUserTxHandler.h"
#include "chain/MinerLiveness.h"
#include "ledger/chain/RecordHandler.h"
#include "ledger/chain/TxFees.h"
#include "ledger/chain/TxLedgerMeta.h"
#include "ledger/chain/TxSignatures.h"
#include "common/Logger.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "common/Serialize.hpp"
#include "ledger/lib/common/Utilities.h"
#include <limits>
#include "ledger/consensus/EpochSeed.h"
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

bool Chain::isSlotSealTime(uint64_t slot) const {
  return txContext_.consensus.isSlotSealTime(slot);
}

bool Chain::isInDeliveryWindow(uint64_t slot) const {
  return txContext_.consensus.isInDeliveryWindow(slot);
}

uint64_t Chain::getProductionSlot() const {
  return txContext_.consensus.getProductionSlot();
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

Chain::Roe<void> Chain::sealReadiness(uint64_t slot) const {
  if (admissionModeFor(getNextBlockId()) != chain_block::BlockAdmissionMode::Full) {
    return Error(E_BLOCK_VALIDATION, "Cannot seal while warming up from a checkpoint (until the next one)");
  }
  const uint64_t first = txContext_.ledger.getStartingBlockId();
  if (first == 0 || !txContext_.optChainConfig.has_value()) {
    return {};
  }
  auto earliest = txContext_.ledger.readBlock(first);
  if (!earliest) {
    return Error(E_BLOCK_NOT_FOUND, "First held block not found: " + earliest.error().message);
  }
  const int64_t windowStart = txContext_.consensus.getSlotStartTime(slot) -
                              static_cast<int64_t>(txContext_.optChainConfig->maxValidationTimespanSeconds);
  if (earliest.value().block.timestamp > windowStart) {
    return Error(E_BLOCK_VALIDATION,
                 "Cannot seal yet: this node holds blocks from " + std::to_string(first) +
                     " only, and a transaction valid now may have been committed before them");
  }
  return {};
}

bool Chain::checkpointEligibleAt(uint64_t index, uint64_t epoch) const {
  return txContext_.optChainConfig.has_value() &&
         chain_block::checkpointEligible(txContext_.ledger, txContext_.consensus, txContext_.optChainConfig.value(),
                                         txContext_.checkpoint, index, epoch);
}

Chain::Checkpoint Chain::getCheckpoint() const { return txContext_.checkpoint; }

uint64_t Chain::getNextBlockId() const {
  return txContext_.ledger.getNextBlockId();
}

int64_t Chain::getConsensusTimestamp() const {
  return txContext_.consensus.getTimestamp();
}

int64_t Chain::getConsensusTimestampMs() const {
  return txContext_.consensus.getTimestampMs();
}

int64_t Chain::getSlotEndTime(uint64_t slot) const {
  return txContext_.consensus.getSlotEndTime(slot);
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

uint64_t Chain::getMaxBlockBytes() const {
  return txContext_.optChainConfig.has_value() ? txContext_.optChainConfig.value().maxBlockBytes : 0;
}

uint64_t Chain::estimateBlockShellBytes(uint64_t slot) const {
  auto last = readLastBlock();
  if (!last) {
    return 0;
  }
  Ledger::ChainNode shell = linkNextBlock(last.value(), slot, 0, 0, {});
  shell.block.epoch = txContext_.consensus.getEpochFromSlot(slot);
  for (std::string *root : {&shell.block.txRoot, &shell.block.stateRoot, &shell.block.stakeSnapshotHash,
                            &shell.block.epochSeed}) {
    root->assign(32, '\0');
  }
  if (chain_block::opensEpoch(txContext_.ledger, shell.block)) {
    shell.block.committee.resize(txContext_.consensus.getStakeholderCount() + 64);
  }
  return shell.block.ltsToString().size();
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

Chain::Roe<std::vector<StateSnapshot::Account>> Chain::exportAccounts() const {
  std::vector<StateSnapshot::Account> out;
  // Every account (all were last written before the end of time), by ascending id.
  for (uint64_t id : txContext_.bank.getAccountIdsBeforeBlockId(std::numeric_limits<uint64_t>::max())) {
    auto held = txContext_.bank.getAccount(id);
    auto full = getAccount(id);
    if (!held || !full) {
      return Error(E_ACCOUNT_NOT_FOUND, "Account " + std::to_string(id) + " cannot be read: " +
                                            (held ? full.error().message : held.error().message));
    }
    if (AccountBuffer::attachmentHashOf(full.value().meta) != held.value().attachmentHash) {
      return Error(E_INTERNAL, "Account " + std::to_string(id) + ": its recorded attachment does not match its hash");
    }
    StateSnapshot::Account account;
    account.id = id;
    account.wallet = held.value().wallet;
    account.blockId = held.value().blockId;
    account.attachment = full.value().meta;
    out.push_back(std::move(account));
  }
  return out;
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
Chain::createRenewalTx(uint64_t accountId, uint64_t slot) const {
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
  // System accounts renew fee-free (validators require fee 0) and are never
  // terminated: the recycle account usually holds nothing (ending it would
  // break every later write-off), and genesis is the native token's issuer,
  // whose "fee" would be minted from its negative balance.
  const bool feeFree = AccountIds::isSystemAccount(accountId);
  const uint64_t minimumFee = feeFree ? 0 : minimumFeeResult.value();

  if (!AccountIds::isSystemAccount(accountId)) {
    auto balance =
        txContext_.bank.getBalance(accountId, AccountBuffer::ID_GENESIS);
    if (balance < static_cast<int64_t>(minimumFee)) {
      // Insufficient balance for renewal, terminate account with whatever
      // balance remains. Fee is 0 here; all remaining balances are transferred
      // to recycle account.
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
    if (accountId == AccountBuffer::ID_GENESIS && txContext_.optChainConfig.has_value()) {
      // The latest genesis meta may be a pending T_CONFIG: carry the config in force for this
      // slot's block and the update separately.
      GenesisAccountMeta gm;
      if (!gm.ltsFromString(tx.meta)) {
        return Error(E_INTERNAL_DESERIALIZE, "Failed to deserialize genesis renewal meta");
      }
      setStateInForce(gm, txContext_.consensus.getEpochFromSlot(slot));
      tx.meta = gm.ltsToString();
    }
  }
  Ledger::Record rec;
  rec.type = type;
  if (type == Ledger::T_END_USER) {
    Ledger::TxEndUser endTx;
    endTx.walletId = tx.walletId;
    endTx.fee = tx.fee;
    // The balances it leaves to the recycle account (a node warming up may not hold the account).
    endTx.meta = EndUserTxHandler::writtenOffMeta(account.wallet.mBalances);
    rec.data = utl::binaryPack(endTx);
  } else {
    rec.data = utl::binaryPack(tx);
  }
  rec.signatures = {};
  return rec;
}

Chain::Roe<uint64_t>
Chain::calculateMaxBlockIdForRenewal(uint64_t atBlockId, uint64_t atSlot) const {
  return mapTx(chain_block::calculateMaxBlockIdForRenewal(
      txContext_.ledger, txContext_.consensus, txContext_.optChainConfig,
      txContext_.checkpoint, atBlockId, atSlot));
}

Chain::Roe<std::vector<Ledger::Record>>
Chain::collectRenewals(uint64_t slot) const {
  std::vector<Ledger::Record> renewals;
  const uint64_t nextBlockId = txContext_.ledger.getNextBlockId();

  auto maxBlockIdResult = calculateMaxBlockIdForRenewal(nextBlockId, slot);
  if (!maxBlockIdResult) {
    return maxBlockIdResult.error();
  }
  const uint64_t maxBlockIdForRenewal = maxBlockIdResult.value();
  std::vector<uint64_t> due;
  if (maxBlockIdForRenewal > 0) {
    due = txContext_.bank.getAccountIdsBeforeBlockId(maxBlockIdForRenewal);
  }
  // A checkpoint block opens with the genesis renewal, due or not (beacon-authority ba5b) — once
  // every account last written before the current checkpoint renewed (B035); until then the
  // genesis renewal waits, and the checkpoint with it.
  if (checkpointEligibleAt(nextBlockId, txContext_.consensus.getEpochFromSlot(slot))) {
    const auto genesis = std::find(due.begin(), due.end(), AccountBuffer::ID_GENESIS);
    if (chain_block::renewalsWaitBelow(txContext_.bank, txContext_.checkpoint.currentId)) {
      if (genesis != due.end()) {
        due.erase(genesis);
      }
    } else if (genesis == due.end()) {
      due.push_back(AccountBuffer::ID_GENESIS);
    }
  }
  if (due.empty()) {
    return renewals;
  }

  // A renewal records the account's committed balance; validators check it
  // against the in-block balance when it applies. System accounts are
  // credited by other records in the block (fees, write-offs), so they renew
  // first, before anything can credit them — genesis first of all — then everyone else.
  std::stable_partition(due.begin(), due.end(), [](uint64_t id) { return AccountIds::isSystemAccount(id); });
  std::stable_partition(due.begin(), due.end(), [](uint64_t id) { return id == AccountBuffer::ID_GENESIS; });
  for (uint64_t accountId : due) {
    auto renewalResult = createRenewalTx(accountId, slot);
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
  size_t outBytes = 0;
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
    std::vector<Ledger::Record> matches;
    size_t matchBytes = 0;
    for (auto it = recs.rbegin(); it != recs.rend(); ++it) {
      if (recordHandler_.matchesWalletForIndex(*it, walletId)) {
        matchBytes += utl::binaryPack(*it).size();
        matches.push_back(*it);
      }
    }
    // A reply stays within what one block may take (B034): a block's matches always fit, whole
    // blocks per page; the next page starts at the block left out.
    if (!out.empty() && outBytes + matchBytes > chain_tx::kMaxBlockBytesCeiling) {
      ioBlockId = currentBlockId + 1;
      return out;
    }
    outBytes += matchBytes;
    out.insert(out.end(), std::make_move_iterator(matches.begin()), std::make_move_iterator(matches.end()));
    ++nBlocksScanned;
    if (out.size() >= THRESHOLD_TXES_FOR_WALLET_TX) {
      break;
    }
  }
  ioBlockId = currentBlockId;
  return out;
}

Chain::Roe<Client::ProducerStatsResponse> Chain::producerStats(uint64_t fromBlockId, uint64_t toBlockId) const {
  const uint64_t first = std::max<uint64_t>(fromBlockId, 1);
  const uint64_t end = std::min(toBlockId, txContext_.ledger.getNextBlockId());
  if (first < end && first < txContext_.ledger.getStartingBlockId()) {
    return Error(E_BLOCK_NOT_FOUND, "This node holds blocks from " +
                                        std::to_string(txContext_.ledger.getStartingBlockId()) + " on");
  }
  Client::ProducerStatsResponse response;
  response.fromBlockId = fromBlockId;
  std::map<uint64_t, Client::ProducerStats> byAccount;
  uint64_t id = first;
  for (; id < end && id - first < Client::kMaxProducerStatsBlocks; ++id) {
    auto read = txContext_.ledger.readBlock(id);
    if (!read) {
      return Error(E_BLOCK_NOT_FOUND, "Block not found: " + std::to_string(id));
    }
    const auto &block = read.value().block;
    auto &producer = byAccount[block.slotLeader];
    producer.accountId = block.slotLeader;
    ++producer.blocks;
    for (const auto &record : block.records) {
      auto typed = record.decode();
      if (!typed) {
        continue;
      }
      const uint64_t fee = std::visit([](const auto &tx) -> uint64_t { return tx.fee; }, typed.value());
      producer.fees += fee;
      response.fees += fee;
    }
    if (auto misses = chargedMisses_.find(id); misses != chargedMisses_.end()) {
      for (uint64_t leader : misses->second) {
        auto &missed = byAccount[leader];
        missed.accountId = leader;
        ++missed.missesCharged;
      }
    }
    if (response.blocks == 0) {
      response.firstSlot = block.slot;
    }
    response.lastSlot = block.slot;
    ++response.blocks;
  }
  response.nextBlockId = id;
  for (auto &[account, stats] : byAccount) {
    response.producers.push_back(stats);
  }
  return response;
}

namespace {

/** The non-empty siblings of a path and the mask saying which heights they are (proofs leave the rest out). */
void packPath(const std::vector<std::string> &siblings, uint64_t &mask, std::vector<std::string> &out) {
  for (size_t h = 0; h < siblings.size(); ++h) {
    if (!AccountStateTree::isEmptyAt(static_cast<int>(h), siblings[h])) {
      mask |= 1ULL << h;
      out.push_back(siblings[h]);
    }
  }
}

} // namespace

std::optional<AccountBuffer::DomainEntry> Chain::findDomain(const std::string &domain) const {
  return txContext_.bank.findDomain(domain);
}

Chain::Roe<Client::DomainProof> Chain::domainProof(const std::string &domain) const {
  if (!chain_domain::isWellFormed(domain)) {
    return Error(E_INVALID_ARGUMENT, "Not a domain: 1 to 7 capital letters A-Z");
  }
  const uint64_t next = txContext_.ledger.getNextBlockId();
  if (next == 0) {
    return Error(E_BLOCK_NOT_FOUND, "No blocks yet");
  }
  auto tip = txContext_.ledger.readBlock(next - 1);
  if (!tip) {
    return Error(E_BLOCK_NOT_FOUND, "Cannot read the tip block: " + tip.error().message);
  }
  const auto &bank = txContext_.bank;
  if (bank.calculateStateRoot() != tip.value().block.stateRoot) {
    return Error(E_ACCOUNT_BUFFER, "The account state is not the tip's right now (a block is being sealed): ask again");
  }
  Client::DomainProof proof;
  proof.domain = domain;
  proof.blockId = tip.value().block.index;
  proof.stateRoot = tip.value().block.stateRoot;
  if (auto bound = bank.findDomain(domain)) {
    proof.present = true;
    proof.accountId = bound->accountId;
  }
  packPath(bank.domainSiblings(domain), proof.siblingMask, proof.siblings);
  return proof;
}

Chain::Roe<Client::AccountProof> Chain::accountProof(uint64_t accountId) const {
  const uint64_t next = txContext_.ledger.getNextBlockId();
  if (next == 0) {
    return Error(E_BLOCK_NOT_FOUND, "No blocks yet");
  }
  auto tip = txContext_.ledger.readBlock(next - 1);
  if (!tip) {
    return Error(E_BLOCK_NOT_FOUND, "Cannot read the tip block: " + tip.error().message);
  }
  const auto &bank = txContext_.bank;
  if (bank.calculateStateRoot() != tip.value().block.stateRoot) {
    return Error(E_ACCOUNT_BUFFER, "The account state is not the tip's right now (a block is being sealed): ask again");
  }
  Client::AccountProof proof;
  proof.accountId = accountId;
  proof.blockId = tip.value().block.index;
  proof.stateRoot = tip.value().block.stateRoot;
  if (auto account = bank.getAccount(accountId)) {
    proof.present = true;
    proof.wallet = account.value().wallet;
    proof.lastWriteBlockId = account.value().blockId;
    proof.attachmentHash = account.value().attachmentHash;
  }
  packPath(bank.stateSiblings(accountId), proof.siblingMask, proof.siblings);
  return proof;
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

Ledger::ChainNode Chain::linkNextBlock(
    const Ledger::ChainNode &previous, uint64_t slot, uint64_t slotLeader,
    int64_t timestamp, std::vector<Ledger::Record> records) const {
  Ledger::ChainNode block;
  block.block.index = previous.block.index + 1;
  block.block.previousHash = previous.hash;
  block.block.slot = slot;
  block.block.slotLeader = slotLeader;
  block.block.timestamp = timestamp;
  block.block.txIndex =
      previous.block.txIndex + previous.block.records.size();
  block.block.records = std::move(records);
  return block;
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
  } else if (isImportBlock(block)) {
    // Like genesis: no committee, the genesis seed; slot 0 (no stake snapshot is taken here, so
    // epoch 0's is the imported miners', at the first produced block).
    block.block.epoch = 0;
    block.block.stakeSnapshotHash = chain_block::calculateStakeSnapshotHash({});
    block.block.committee.clear();
    auto genesis = txContext_.ledger.readBlock(0);
    if (!genesis) {
      return Error(E_BLOCK_NOT_FOUND, "Import block without its genesis: " + genesis.error().message);
    }
    block.block.epochSeed = genesis.value().block.epochSeed;
  } else {
    block.block.epoch =
        txContext_.consensus.getEpochFromSlot(block.block.slot);
    refreshStakeholders(block.block.slot);
    block.block.stakeSnapshotHash = chain_block::calculateStakeSnapshotHash(
        txContext_.consensus.getStakeholders());
    block.block.committee.clear();
    if (chain_block::opensEpoch(txContext_.ledger, block.block)) {
      block.block.committee = chain_block::toCommittee(txContext_.consensus.getStakeholders());
    }
    auto seedRoe = ensureBlockEpochSeed(block);
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

  // Slot checks run before any tip mutation so a bad slot fails cleanly.
  if (block.block.index > 0 && !isImportBlock(block)) {
    auto arrival = mapTxVoid(chain_block::checkBlockArrival(
        block, txContext_.ledger, txContext_.consensus));
    if (!arrival) {
      return Error(E_BLOCK_VALIDATION, arrival.error().message);
    }
  }

  auto snapshot = beginTipUpdate();
  auto sealed = sealOnTip(block);
  if (!sealed) {
    rollbackTipUpdate(std::move(snapshot));
    return sealed;
  }
  sealSnapshot_ = std::move(snapshot);
  pendingSeal_ = PendingSeal{block.block.index, block.hash};
  return {};
}

void Chain::abandonSeal() {
  if (sealSnapshot_.has_value()) {
    rollbackTipUpdate(std::move(*sealSnapshot_));
    sealSnapshot_.reset();
  }
  pendingSeal_.reset();
}

Chain::TipSnapshot Chain::beginTipUpdate() {
  txContext_.bank.beginOverlay();
  return TipSnapshot{txContext_.consensus.saveTipState(), txContext_.optChainConfig,
                     txContext_.pendingChainConfig, txContext_.issuanceBaseline, txContext_.checkpoint,
                     txContext_.optAuthority, txContext_.importProgress};
}

void Chain::commitTipUpdate() { txContext_.bank.commitOverlay(); }

void Chain::rollbackTipUpdate(TipSnapshot snapshot) {
  txContext_.bank.rollbackOverlay();
  txContext_.consensus.restoreTipState(std::move(snapshot.consensus));
  txContext_.optChainConfig = std::move(snapshot.optChainConfig);
  txContext_.pendingChainConfig = std::move(snapshot.pendingChainConfig);
  txContext_.issuanceBaseline = snapshot.issuanceBaseline;
  txContext_.checkpoint = snapshot.checkpoint;
  txContext_.optAuthority = std::move(snapshot.optAuthority);
  txContext_.importProgress = std::move(snapshot.importProgress);
}

Chain::Roe<void> Chain::sealOnTip(Ledger::ChainNode &block) {
  // Assemble → Check (non-genesis Full layers) → Apply (always Full) →
  // Commit-hold (pendingSeal). Do not use admissionModeFor for seal apply:
  // late-join CheckpointReplay is for trusted catch-up ingest only.
  auto assembled = assembleBlockHeader(block);
  if (!assembled) {
    return assembled;
  }

  const bool importing = isImportBlock(block);
  if (importing) {
    if (auto valid = mapTxVoid(chain_block::validateImportBlockBody(
            block, txContext_.ledger, txContext_.optChainConfig.value(),
            txContext_.importProgress->accountCount - txContext_.importProgress->imported));
        !valid) {
      return Error(E_BLOCK_VALIDATION, valid.error().message);
    }
  } else if (block.block.index > 0) {
    enterBlockEpoch(block.block.epoch);
    if (auto ready = sealReadiness(block.block.slot); !ready) {
      return ready;
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
  if (importing) {
    if (auto finished = finishImportBlock(); !finished) {
      return finished;
    }
  } else if (block.block.index > 0) {
    if (auto liveness = applyMinerLiveness(block, chain_block::BlockAdmissionMode::Full); !liveness) {
      return liveness;
    }
    applyDueAccountRecoveries(block);
  }

  if (block.block.index == 0) {
    // Install after genesis apply (init clears any earlier seed).
    txContext_.consensus.setEpochSeed(0, block.block.epochSeed);
  }

  block.block.stateRoot = txContext_.bank.calculateStateRoot();
  block.block.finalAt = expectedFinalAt(block.block);
  block.hash = calculateHash(block.block);
  return {};
}

Chain::Roe<void> Chain::applyMinerLiveness(const Ledger::ChainNode &block, chain_block::BlockAdmissionMode mode) {
  if (!txContext_.optChainConfig.has_value() || block.block.index == 0) {
    return {};
  }
  uint64_t prevSlot = 0;
  if (auto prev = txContext_.ledger.readBlock(block.block.index - 1)) {
    prevSlot = prev.value().block.slot;
  } else {
    // Late join starts at a checkpoint, whose previous block is not held. A checkpoint opens its
    // epoch, so the previous block lies in an earlier epoch and the skipped slots charged are
    // exactly those from the epoch's first slot: any previous slot before it gives the same.
    if (chain_block::admissionTxStrict(mode)) {
      return Error(E_BLOCK_NOT_FOUND, "Previous block not found for miner liveness: " + prev.error().message);
    }
    const uint64_t epochFirstSlot = block.block.epoch * txContext_.consensus.getConfig().slotsPerEpoch;
    prevSlot = epochFirstSlot > 0 ? epochFirstSlot - 1 : 0;
  }
  // The first block after a beacon outage commits the new beacon's certificate: its skipped slots
  // are the outage, not missed leaders (bp3).
  const bool chargeSkipped = !BeaconCertTxHandler::commitsCutover(block.block);
  std::vector<uint64_t> charged;
  auto applied = mapTxVoid(chain_tx::applyMinerLiveness(txContext_.bank, txContext_.consensus,
                                                        txContext_.optChainConfig.value(), prevSlot,
                                                        block.block, chain_block::admissionTxStrict(mode),
                                                        chargeSkipped, &charged));
  if (!applied) {
    return Error(E_TX_VALIDATION, "Miner liveness: " + applied.error().message);
  }
  if (charged.empty()) {
    chargedMisses_.erase(block.block.index);
  } else {
    chargedMisses_[block.block.index] = std::move(charged);
  }
  return {};
}

void Chain::applyDueAccountRecoveries(const Ledger::ChainNode &block) {
  // At the first block of an epoch (or the first this node holds), after its records: the set's
  // withdrawal in the block clears a recovery before it applies; renewals there carry the keys before.
  if (block.block.index > 0) {
    if (auto prev = txContext_.ledger.readBlock(block.block.index - 1); prev && prev.value().block.epoch == block.block.epoch) {
      return;
    }
  }
  for (uint64_t id : txContext_.bank.applyDueRecoveries(block.block.epoch)) {
    log().info << "Account " << id << ": its recovery applied at block " << block.block.index
               << " (epoch " << block.block.epoch << "): the announced keys sign for it now";
  }
}

bool Chain::isMinerDue(uint64_t accountId, uint64_t slot) const {
  if (!txContext_.optChainConfig.has_value()) {
    return false;
  }
  auto account = txContext_.bank.getAccount(accountId);
  return account && chain_tx::isMinerDue(account.value().wallet.miner, slot, txContext_.optChainConfig.value());
}

std::vector<consensus::Stakeholder> Chain::electionCandidates() const {
  const uint64_t minStake =
      txContext_.optChainConfig.has_value() ? txContext_.optChainConfig->minerMinStake : 0;
  return txContext_.bank.getStakeholders(minStake);
}

void Chain::refreshStakeholders() {
  const uint64_t clockEpoch = txContext_.consensus.getCurrentEpoch();
  if (auto recorded = epochStakeSnapshots_.find(clockEpoch); recorded != epochStakeSnapshots_.end()) {
    txContext_.consensus.setStakeholders(recorded->second, clockEpoch);
  } else if (txContext_.consensus.isStakeUpdateNeeded()) {
    // Provisional (leader election before this epoch's first block). Not
    // recorded: blocks of earlier epochs may still arrive and change it.
    txContext_.consensus.setStakeholders(electionCandidates(), clockEpoch);
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
  if (clockEpoch != tipEpoch &&
      !txContext_.consensus.hasEpochSeedFor(clockEpoch)) {
    (void)ensureEpochSeed(clockEpoch);
  }
}

void Chain::recordEpochStakeSnapshot(uint64_t epoch, std::vector<consensus::Stakeholder> stakeholders) {
  epochStakeSnapshots_.erase(epochStakeSnapshots_.lower_bound(epoch), epochStakeSnapshots_.end());
  epochStakeSnapshots_.emplace(epoch, std::move(stakeholders));
}

void Chain::refreshStakeholders(uint64_t blockSlot) {
  const uint64_t epoch = txContext_.consensus.getEpochFromSlot(blockSlot);
  // Snapshots recorded for later epochs predate this block: drop them.
  epochStakeSnapshots_.erase(epochStakeSnapshots_.upper_bound(epoch), epochStakeSnapshots_.end());
  auto recorded = epochStakeSnapshots_.find(epoch);
  if (recorded == epochStakeSnapshots_.end()) {
    // First block of this epoch on this tip: the bank holds the state after
    // the last block of earlier epochs — the same on every node.
    recorded = epochStakeSnapshots_.emplace(epoch, electionCandidates()).first;
    if (epoch > kStakeSnapshotsKept) {
      epochStakeSnapshots_.erase(epochStakeSnapshots_.begin(),
                                 epochStakeSnapshots_.lower_bound(epoch - kStakeSnapshotsKept));
    }
  }
  txContext_.consensus.setStakeholders(recorded->second, epoch);
}

std::vector<std::string>
Chain::collectPrevEpochLookbackHashes(uint64_t prevEpoch, uint64_t endBlockId) const {
  std::vector<std::string> newestFirst;
  const uint64_t nextId = std::min(endBlockId, txContext_.ledger.getNextBlockId());
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

Chain::Roe<std::pair<uint64_t, std::string>> Chain::readLatestEpochSeed(uint64_t maxEpoch, uint64_t endBlockId) const {
  const uint64_t nextId = std::min(endBlockId, txContext_.ledger.getNextBlockId());
  for (uint64_t id = nextId; id > 0; --id) {
    auto blockRoe = txContext_.ledger.readBlock(id - 1);
    if (!blockRoe) {
      break;
    }
    if (blockRoe->block.epoch <= maxEpoch &&
        blockRoe->block.epochSeed.size() == utl::SHA256_DIGEST_SIZE) {
      return std::make_pair(blockRoe->block.epoch, blockRoe->block.epochSeed);
    }
  }
  return Error(E_STATE_INIT, "No stored epochSeed at or before epoch " + std::to_string(maxEpoch));
}

std::string Chain::deriveNextEpochSeed(uint64_t epoch, const std::string &prevSeed, const std::string &stakeHash,
                                       uint64_t endBlockId) const {
  const std::string networkId =
      txContext_.optChainConfig.has_value() ? txContext_.optChainConfig->networkId : std::string{};
  // No blocks in epoch - 1 → no lookback hashes → empty-prev tip material.
  auto lookback = collectPrevEpochLookbackHashes(epoch - 1, endBlockId);
  const std::string tipMaterial = consensus::lookbackTipMaterial(epoch - 1, prevSeed, lookback);
  return consensus::deriveEpochSeed(epoch, networkId, prevSeed, tipMaterial, stakeHash);
}

Chain::Roe<void> Chain::ensureEpochSeed(uint64_t epoch, std::optional<uint64_t> endBlockId) {
  if (txContext_.consensus.hasEpochSeedFor(epoch)) {
    return {};
  }
  const uint64_t end = endBlockId.value_or(txContext_.ledger.getNextBlockId());

  if (epoch == 0) {
    if (!txContext_.optChainConfig.has_value()) {
      return Error(E_STATE_INIT,
                   "Cannot derive genesis epoch seed without chain config");
    }
    const std::string cfgDigest =
        chain_block::calculateGenesisConfigDigest(*txContext_.optChainConfig);
    const std::string seed =
        consensus::deriveGenesisEpochSeed(txContext_.optChainConfig->networkId, cfgDigest);
    txContext_.consensus.setEpochSeed(0, seed);
    return {};
  }

  // Start from the newest stored seed before this epoch. Epochs after it had no blocks: each
  // seed follows from the one before (empty-prev tip material), so a chain survives idle epochs.
  auto base = readLatestEpochSeed(epoch - 1, end);
  if (!base) {
    return Error(base.error().code, base.error().message);
  }
  if (epoch - base.value().first > consensus::kMaxEmptyEpochs + 1) {
    return Error(E_STATE_INIT, "Epoch " + std::to_string(epoch) + " is more than " +
                                   std::to_string(consensus::kMaxEmptyEpochs) +
                                   " empty epochs past the last block's epoch " +
                                   std::to_string(base.value().first));
  }
  // Each epoch's own snapshot, not whatever is installed: the wall-clock refresh installs the
  // clock epoch's (provisional) stakes before deriving the tip epoch's seed. Epochs with no
  // recorded snapshot share the provisional one (the bank does not change without blocks).
  std::optional<std::string> provisionalStakeHash;
  std::string seed = base.value().second;
  for (uint64_t next = base.value().first + 1; next <= epoch; ++next) {
    std::string stakeHash;
    if (auto recorded = epochStakeSnapshots_.find(next); recorded != epochStakeSnapshots_.end()) {
      stakeHash = chain_block::calculateStakeSnapshotHash(recorded->second);
    } else {
      if (!provisionalStakeHash) {
        provisionalStakeHash = chain_block::calculateStakeSnapshotHash(electionCandidates());
      }
      stakeHash = *provisionalStakeHash;
    }
    seed = deriveNextEpochSeed(next, seed, stakeHash, end);
  }
  txContext_.consensus.setEpochSeed(epoch, seed);
  return {};
}

Chain::Roe<void> Chain::ensureBlockEpochSeed(const Ledger::ChainNode &block) {
  const uint64_t epoch = block.block.epoch;
  bool firstOfEpoch = true;
  if (block.block.index > 0) {
    auto prev = txContext_.ledger.readBlock(block.block.index - 1);
    firstOfEpoch = !prev || prev->block.epoch < epoch;
  }
  if (firstOfEpoch && txContext_.consensus.hasEpochSeedFor(epoch)) {
    txContext_.consensus.clearEpochSeed();
  }
  return ensureEpochSeed(epoch, block.block.index); // from the blocks before it, not the ledger's tip
}

void Chain::initConsensus(const consensus::SlotCommittee::Config &config) {
  txContext_.consensus.init(config);
  epochStakeSnapshots_.clear();
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
  authoritySteps_.clear();
  authorityStepsFrom_ = config.startingBlockId;
  chargedMisses_.clear();
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

  if (sealSnapshot_.has_value()) {
    rollbackTipUpdate(std::move(*sealSnapshot_));
    sealSnapshot_.reset();
  }
  log().info << "Resetting account buffer";
  txContext_.bank.reset();
  txContext_.importProgress.reset();
  epochStakeSnapshots_.clear();
  chargedMisses_.clear();
  pendingSeal_.reset();
  authoritySteps_.clear();
  authorityStepsFrom_ = startingBlockId;

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

    if (auto prepared = prepareBlockEpoch(block, replayMode); !prepared) {
      return Error(prepared.error().code, "Block " + std::to_string(blockId) + ": " + prepared.error().message);
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
    noteAuthoritySteps(block);

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

  // Live tip admission only ever advances the chain by exactly one block.
  // Accepting index < nextBlockId here would let an already-committed (or
  // forged) historical index be re-applied on top of the current, more
  // advanced tip state -- a replay that double-applies old transactions.
  // (checkBlockStructural's validateBlockSequence stays permissive because
  // it is shared with full-history replay from disk, where every block
  // legitimately has index < the ledger's final block count.)
  const uint64_t expectedIndex = txContext_.ledger.getNextBlockId();
  if (block.block.index != expectedIndex) {
    return Error(E_BLOCK_VALIDATION,
                 "Invalid block index: expected " +
                     std::to_string(expectedIndex) + " got " +
                     std::to_string(block.block.index));
  }

  // Slot monotonicity / future bound: arriving blocks under Full admission
  // only. loadFromLedger and CheckpointReplay catch-up skip them so history
  // written before these rules still loads.
  if (block.block.index > 0 && !isImportBlock(block) &&
      admissionModeFor(block.block.index) ==
          chain_block::BlockAdmissionMode::Full) {
    auto arrival = mapTxVoid(chain_block::checkBlockArrival(
        block, txContext_.ledger, txContext_.consensus));
    if (!arrival) {
      return Error(E_BLOCK_VALIDATION, arrival.error().message);
    }
  }

  // Stake/seed refresh, apply and stateRoot check all run on the tip; any
  // failure restores the pre-block state.
  auto snapshot = beginTipUpdate();
  auto applied = applyNextBlock(block);
  if (!applied) {
    rollbackTipUpdate(std::move(snapshot));
    return applied;
  }
  commitTipUpdate();
  noteAuthoritySteps(block);

  log().info << "Block added: " << block.block.index
             << " from slot leader: " << block.block.slotLeader;

  return {};
}

Chain::Roe<void> Chain::applyNextBlock(const Ledger::ChainNode &block) {
  const auto admissionMode = admissionModeFor(block.block.index);
  if (auto prepared = prepareBlockEpoch(block, admissionMode); !prepared) {
    return prepared;
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
  return {};
}

Chain::Roe<void> Chain::commitSealedBlock(const Ledger::ChainNode &block) {
  auto committed = commitSealedOnTip(block);
  pendingSeal_.reset();
  if (sealSnapshot_.has_value()) {
    if (committed) {
      commitTipUpdate();
    } else {
      rollbackTipUpdate(std::move(*sealSnapshot_));
    }
    sealSnapshot_.reset();
  }
  if (!committed) {
    return committed;
  }
  noteAuthoritySteps(block);
  log().info << "Block added: " << block.block.index
             << " from slot leader: " << block.block.slotLeader;
  return {};
}

Chain::Roe<void> Chain::commitSealedOnTip(const Ledger::ChainNode &block) {
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
  if (auto finality = checkFinalAt(block.block); !finality) {
    return finality;
  }
  noteFinalAt(block.block);

  // Rotate before persist, as the processNormalBlock → addBlock path does (the rule reads the
  // block before this one from the ledger).
  maybeRotateCheckpoint(block);

  auto ledgerResult = txContext_.ledger.addBlock(block);
  if (!ledgerResult) {
    return Error(E_LEDGER_WRITE,
                 "Failed to persist block: " + ledgerResult.error().message);
  }

  if (block.block.epochSeed.size() == utl::SHA256_DIGEST_SIZE) {
    txContext_.consensus.setEpochSeed(block.block.epoch, block.block.epochSeed);
  }
  return {};
}

Chain::Roe<void> Chain::prepareBlockEpoch(const Ledger::ChainNode &block, chain_block::BlockAdmissionMode mode) {
  // Genesis installs consensus while its record applies; import blocks take no stake snapshot.
  if (block.block.index == 0 || isImportBlock(block)) {
    return {};
  }
  if (!txContext_.optChainConfig.has_value()) {
    if (auto installed = installCheckpointState(block); !installed) {
      return installed;
    }
  }
  if (!chain_block::admissionRunsConsensusAndBody(mode)) {
    // Trusted catch-up: the node may hold only some accounts, so it takes each epoch's committee
    // from the block opening it (checked against stakeSnapshotHash) and the block's own seed (the
    // epochs before it may not be held). Leaders, and the liveness charges they drive, are exact.
    if (chain_block::opensEpoch(txContext_.ledger, block.block)) {
      if (auto committee = mapTxVoid(chain_block::validateBlockCommittee(txContext_.ledger, block.block));
          !committee) {
        return committee;
      }
      recordEpochStakeSnapshot(block.block.epoch, chain_block::fromCommittee(block.block.committee));
    }
    refreshStakeholders(block.block.slot);
    if (block.block.epochSeed.size() == utl::SHA256_DIGEST_SIZE) {
      txContext_.consensus.setEpochSeed(block.block.epoch, block.block.epochSeed);
    }
    return {};
  }
  refreshStakeholders(block.block.slot);
  if (auto seeded = ensureBlockEpochSeed(block); !seeded) {
    return Error(E_BLOCK_VALIDATION, "Failed to ensure epoch seed: " + seeded.error().message);
  }
  return {};
}

Chain::Roe<void> Chain::installCheckpointState(const Ledger::ChainNode &block) {
  if (block.block.records.empty() || !chain_block::isGenesisRenewal(block.block.records.front())) {
    return Error(E_STATE_INIT, "Block " + std::to_string(block.block.index) +
                                   " is not a checkpoint: it does not open with the genesis renewal");
  }
  auto tx = utl::binaryUnpack<Ledger::TxRenewal>(block.block.records.front().data);
  GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    return Error(E_INTERNAL_DESERIALIZE, "Unreadable genesis renewal in checkpoint block " +
                                             std::to_string(block.block.index));
  }
  txContext_.optChainConfig = gm.config;
  if (gm.hasPending) {
    txContext_.pendingChainConfig.emplace(gm.pending);
  } else {
    txContext_.pendingChainConfig.reset();
  }
  txContext_.optAuthority = gm.authority;
  // Slot timing is fixed at genesis; this node keeps its clock offset.
  txContext_.consensus.setSlotTiming(gm.config.genesisTime, gm.config.slotDuration, gm.config.slotsPerEpoch);
  epochStakeSnapshots_.clear();
  // The renewal records genesis as the previous epoch left it (it renews first): in place before
  // the block, so this epoch's issuance baseline is right too.
  if (auto placed = mapTxVoid(chain_tx::replaceGenesisAccount(txContext_.bank, block.block.index, gm.genesis.wallet,
                                                                  gm.genesis.meta));
      !placed) {
    return placed;
  }
  log().info << "Starting at checkpoint " << block.block.index
             << ": config, consensus and authority from its genesis renewal (beacon generation "
             << gm.authority.beacon.generation << ")";
  return {};
}

void Chain::maybeRotateCheckpoint(const Ledger::ChainNode &block) {
  if (block.block.index == 0) {
    return;
  }
  if (txContext_.optChainConfig.has_value() &&
      chain_block::rotatesCheckpoint(txContext_.ledger, txContext_.consensus, txContext_.optChainConfig.value(),
                                     txContext_.checkpoint, block)) {
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
  }
  if (isImportBlock(block)) {
    return processImportBlock(block);
  }
  return processNormalBlock(block, mode);
}

bool Chain::isImportBlock(const Ledger::ChainNode &block) const {
  return block.block.index > 0 && txContext_.importProgress.has_value();
}

Chain::Roe<void> Chain::processImportBlock(const Ledger::ChainNode &block) {
  if (auto structural = mapTxVoid(chain_block::checkBlockStructural(block, txContext_.ledger)); !structural) {
    return Error(E_BLOCK_VALIDATION, structural.error().message);
  }
  if (auto valid = mapTxVoid(chain_block::validateImportBlockBody(
          block, txContext_.ledger, txContext_.optChainConfig.value(),
          txContext_.importProgress->accountCount - txContext_.importProgress->imported));
      !valid) {
    return Error(E_BLOCK_VALIDATION, valid.error().message);
  }
  for (const auto &rec : block.block.records) {
    auto applied = processNormalTxRecord(rec, block.block.index, 0, 0, chain_block::BlockAdmissionMode::Full);
    if (!applied) {
      return Error(E_TX_VALIDATION, "Import block " + std::to_string(block.block.index) + ": " +
                                        applied.error().message);
    }
  }
  if (auto finished = finishImportBlock(); !finished) {
    return finished;
  }
  if (block.block.stateRoot != txContext_.bank.calculateStateRoot()) {
    return Error(E_BLOCK_HASH, "Import block " + std::to_string(block.block.index) + " stateRoot mismatch");
  }
  if (auto finality = checkFinalAt(block.block); !finality) {
    return finality;
  }
  noteFinalAt(block.block);
  return {};
}

Chain::Roe<void> Chain::finishImportBlock() {
  auto &progress = txContext_.importProgress;
  if (!progress.has_value() || !progress->complete()) {
    return {};
  }
  if (progress->oldLeaves.root() != progress->predecessor.stateRoot) {
    return Error(E_BLOCK_VALIDATION, "The imported accounts do not hash to the state root of block " +
                                         std::to_string(progress->predecessor.height) + " of network '" +
                                         progress->predecessor.networkId + "'");
  }
  log().info << "Imported " << progress->accountCount << " accounts of network '" << progress->predecessor.networkId
             << "' at block " << progress->predecessor.height << ": they hash to its state root";
  progress.reset();
  return {};
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
  if (auto finality = checkFinalAt(block.block); !finality) {
    return finality;
  }
  noteFinalAt(block.block);
  return {};
}

void Chain::enterBlockEpoch(uint64_t epoch) {
  applyDueRecovery(epoch);
  auto &pending = txContext_.pendingChainConfig;
  if (pending.has_value() && epoch >= pending->activationEpoch) {
    txContext_.optChainConfig = std::move(pending->config);
    log().info << "Config update in force from epoch " << pending->activationEpoch << ": "
               << txContext_.optChainConfig.value();
    pending.reset();
  }
  auto &baseline = txContext_.issuanceBaseline;
  if (!baseline.has_value() || baseline->epoch < epoch) {
    baseline = IssuanceBaseline{epoch, txContext_.bank.getBalance(AccountBuffer::ID_GENESIS, AccountBuffer::ID_GENESIS)};
  }
}

std::optional<Client::RecoveryStatus> Chain::recoveryStatus() const {
  if (!txContext_.optAuthority.has_value() || !txContext_.optAuthority->hasPendingRecovery) {
    return std::nullopt;
  }
  const PendingRecovery &pending = txContext_.optAuthority->pendingRecovery;
  Client::RecoveryStatus status;
  status.announcedAt = pending.announcedAt;
  status.applyEpoch = pending.applyEpoch;
  status.adminPublicKeys = pending.announcement.adminPublicKeys;
  status.adminMinSignatures = pending.announcement.adminMinSignatures;
  return status;
}

void Chain::applyDueRecovery(uint64_t epoch) {
  auto &authority = txContext_.optAuthority;
  if (!authority.has_value() || !authority->hasPendingRecovery) {
    return;
  }
  if (epoch < authority->pendingRecovery.applyEpoch) {
    // Visible while it waits: one alert per epoch (B007).
    if (recoveryNoticeEpoch_ != epoch) {
      recoveryNoticeEpoch_ = epoch;
      log().error << "RECOVERY PENDING: announced at block " << authority->pendingRecovery.announcedAt
                  << ", it replaces the admin set at epoch " << authority->pendingRecovery.applyEpoch << " ("
                  << authority->pendingRecovery.applyEpoch - epoch
                  << " epochs from now); the admin set is frozen until then";
    }
    return;
  }
  const PendingRecovery recovery = authority->pendingRecovery;
  authority->hasPendingRecovery = false;
  authority->pendingRecovery = {};
  // In place: the genesis account's blockId (where renewal reads its meta) does not move.
  if (auto replaced = txContext_.bank.setWalletKeys(AccountBuffer::ID_GENESIS,
                                                    recovery.announcement.adminPublicKeys,
                                                    recovery.announcement.adminMinSignatures);
      !replaced) {
    // Held by every node that applies blocks; one that does not (a warm-up before its start) has no
    // genesis account to change.
    log().warning << "Recovery: the genesis account is not held here: " << replaced.error().message;
  }
  log().error << "RECOVERY APPLIED at epoch " << epoch << ": the admin set is replaced ("
              << recovery.announcement.adminPublicKeys.size() << " keys, "
              << static_cast<int>(recovery.announcement.adminMinSignatures) << " signatures), announced at block "
              << recovery.announcedAt;
  // A certificate held out of band was signed by the replaced admin set: check it again.
  refreshSeenCertificate();
}

Chain::Roe<void> Chain::checkProducerSigned(const Ledger::ChainNode &block) const {
  // The elected leader is public: only its account's keys (as held before this block) make a
  // block of its slot, so no one else can take the slot with a block in its name.
  auto producer = txContext_.bank.getAccount(block.block.slotLeader);
  if (!producer) {
    return Error(E_BLOCK_VALIDATION, "Block " + std::to_string(block.block.index) + ": slot leader " +
                                         std::to_string(block.block.slotLeader) + " not found");
  }
  if (auto signed_ = verifySignaturesAgainstAccount(
          Ledger::Block::producerSigningMessage(getNetworkId(), block.hash), block.block.producerSignatures,
          producer.value());
      !signed_) {
    return Error(E_BLOCK_VALIDATION, "Block " + std::to_string(block.block.index) +
                                         " is not signed by its slot leader " +
                                         std::to_string(block.block.slotLeader) + ": " + signed_.error().message);
  }
  return {};
}

Chain::Roe<void> Chain::processNormalBlock(
    const Ledger::ChainNode &block, chain_block::BlockAdmissionMode mode) {
  // A block of a new epoch is checked and applied under that epoch's config.
  enterBlockEpoch(block.block.epoch);
  auto roe = mapTxVoid(chain_block::checkBlock(
      block, mode, txContext_.ledger, txContext_.consensus, txContext_.bank,
      txContext_.optChainConfig, txContext_.checkpoint, recordHandler_));
  if (!roe) {
    return Error(E_BLOCK_VALIDATION, "Block validation failed for block " +
                                         std::to_string(block.block.index) +
                                         ": " + roe.error().message);
  }
  if (chain_block::admissionRunsConsensusAndBody(mode)) {
    if (auto produced = checkProducerSigned(block); !produced) {
      return produced;
    }
  }

  for (const auto &rec : block.block.records) {
    auto result = processNormalTxRecord(rec, block.block.index, block.block.slot,
                                        block.block.slotLeader, mode);
    if (!result) {
      return Error(E_TX_VALIDATION,
                   "Failed to process transaction: " + result.error().message);
    }
  }
  if (auto liveness = applyMinerLiveness(block, mode); !liveness) {
    return liveness;
  }
  applyDueAccountRecoveries(block);

  // A node warming up from a checkpoint holds only the accounts renewed so far: its root cannot
  // match until Full admission (from the next checkpoint), which checks every block.
  if (chain_block::admissionTxStrict(mode)) {
    const std::string expectedStateRoot = txContext_.bank.calculateStateRoot();
    if (block.block.stateRoot != expectedStateRoot) {
      return Error(E_BLOCK_HASH, "Block stateRoot mismatch");
    }
    if (auto finality = checkFinalAt(block.block); !finality) {
      return finality;
    }
  }
  noteFinalAt(block.block);

  maybeRotateCheckpoint(block);

  return {};
}

Chain::Roe<void> Chain::addBufferTransaction(
    AccountBuffer &bank,
    const Ledger::Record &record,
    uint64_t slotLeaderId) const {
  // Against the keys this draft holds: an earlier record in it may have changed them, as the seal will.
  auto roe = validateTxSignatures(record, slotLeaderId,
                                  chain_block::BlockAdmissionMode::Full, &bank);
  if (!roe) {
    return Error(E_TX_SIGNATURE, "Failed to validate buffer transaction: " +
                                     roe.error().message);
  }

  // One key per block (BlockValidation): the cross-block scan below only sees the ledger, so a
  // draft that already holds this key must refuse it here — or the leader could never seal.
  auto keyRoe = recordHandler_.getIdempotencyKey(record);
  if (!keyRoe) {
    return Error(E_TX_IDEMPOTENCY, keyRoe.error().message);
  }
  const auto &key = keyRoe.value();
  if (key && bank.hasIdempotencyKey(*key)) {
    return Error(E_TX_IDEMPOTENCY, "Duplicate idempotent id in block draft: " + std::to_string(key->second) +
                                       " for wallet: " + std::to_string(key->first));
  }

  auto blockId = getNextBlockId();
  // The slot the block would carry: a leader seals the slot that just ended (delivery window).
  const uint64_t productionSlot = getProductionSlot();
  BufferApplyContext ctx{ txContext_,
                          blockId,
                          productionSlot,
                          chain_block::BlockAdmissionMode::Full };
  // A rejected tx must not leave partial effects in the scratch bank.
  bank.beginOverlay();
  auto applied = mapTxVoid(recordHandler_.applyBuffer(record, bank, ctx));
  if (applied) {
    if (key) {
      bank.addIdempotencyKey(*key);
    }
    bank.commitOverlay();
  } else {
    bank.rollbackOverlay();
  }
  return applied;
}

void Chain::setStateInForce(GenesisAccountMeta &gm, uint64_t epoch) const {
  gm.setConfigStateAt(txContext_.optChainConfig.value(), txContext_.pendingChainConfig, epoch);
  if (txContext_.optAuthority.has_value()) {
    // As the block's epoch will hold it: a recovery due by then has replaced the admin set
    // before the records apply (applyDueRecovery).
    ChainAuthority authority = txContext_.optAuthority.value();
    chain_tx::advanceAuthorityToEpoch(authority, gm.genesis.wallet, epoch);
    gm.authority = authority;
  }
}

Chain::Roe<GenesisAccountMeta> Chain::genesisMetaInForce() const {
  if (!txContext_.optChainConfig.has_value()) {
    return Error(E_STATE_INIT, "The chain config is not loaded yet");
  }
  auto admin = getAccount(AccountBuffer::ID_GENESIS);
  if (!admin) {
    return admin.error();
  }
  GenesisAccountMeta gm;
  gm.genesis = admin.value();
  setStateInForce(gm, getCurrentEpoch());
  return gm;
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

Chain::Roe<Chain::BeaconCertificate> Chain::verifyBeaconCertificateRecord(const Ledger::Record &record) const {
  if (record.type != Ledger::T_BEACON_CERT) {
    return Error(E_TX_VALIDATION, "Not a beacon certificate record");
  }
  auto tx = utl::binaryUnpack<Ledger::TxBeaconCert>(record.data);
  if (!tx) {
    return Error(E_TX_VALIDATION, "Invalid beacon certificate record: " + tx.error().message);
  }
  auto cert = BeaconCertTxHandler::decodeCertificate(tx.value());
  if (!cert) {
    return Error(E_TX_VALIDATION, cert.error().message);
  }
  if (!txContext_.optAuthority.has_value()) {
    return Error(E_TX_VALIDATION, "Chain authority not initialized");
  }
  if (auto frozen = chain_tx::requireAdminSetNotFrozen(txContext_.optAuthority.value(), "Beacon certificate");
      !frozen) {
    return Error(E_TX_VALIDATION, frozen.error().message);
  }
  const std::string networkId = getNetworkId();
  if (auto signed_ = verifyAccountSignatures(AccountBuffer::ID_GENESIS, record.signingMessage(networkId),
                                             record.signatures);
      !signed_) {
    return Error(E_TX_SIGNATURE, "Beacon certificate not signed by the admin set: " + signed_.error().message);
  }
  auto admin = txContext_.bank.getAccount(AccountBuffer::ID_GENESIS);
  if (!admin) {
    return Error(E_ACCOUNT_NOT_FOUND, "Genesis account not found");
  }
  if (auto valid = chain_tx::validateNextBeaconCertificate(cert.value(), txContext_.optAuthority.value(),
                                                          admin.value().wallet, networkId, std::nullopt);
      !valid) {
    return Error(E_TX_VALIDATION, valid.error().message);
  }
  return cert.value();
}

Chain::Roe<Chain::CertificateOffer> Chain::offerCertificate(const Ledger::Record &record) {
  if (record.type != Ledger::T_BEACON_CERT) {
    return Error(E_TX_VALIDATION, "Not a beacon certificate record");
  }
  auto tx = utl::binaryUnpack<Ledger::TxBeaconCert>(record.data);
  if (!tx) {
    return Error(E_TX_VALIDATION, "Invalid beacon certificate record: " + tx.error().message);
  }
  auto cert = BeaconCertTxHandler::decodeCertificate(tx.value());
  if (!cert) {
    return Error(E_TX_VALIDATION, cert.error().message);
  }
  if (!txContext_.optAuthority.has_value()) {
    return Error(E_STATE_INIT, "Chain authority not initialized");
  }
  // Order first: only a certificate that would be kept is worth its signature checks.
  const uint64_t generation = cert.value().generation;
  if (generation <= txContext_.optAuthority->beacon.generation ||
      (seenCert_ && generation < seenCert_->generation)) {
    return CertificateOffer::Older;
  }
  const bool sameGeneration = seenCert_ && generation == seenCert_->generation;
  if (sameGeneration && cert.value().ltsToString() == seenCert_->ltsToString()) {
    return CertificateOffer::Same;
  }
  auto verified = verifyBeaconCertificateRecord(record);
  if (!verified) {
    return verified.error();
  }
  if (sameGeneration) {
    log().warning << "Another admin-signed beacon certificate for generation " << generation
                  << " than the one held: keeping the one held";
    return CertificateOffer::Conflict;
  }
  seenCert_ = verified.value();
  seenCertRecord_ = record;
  refreshEquivocation();
  if (seenCert_->cutoverHeight == UINT64_MAX) {
    log().info << "Holding beacon certificate generation " << generation << " (no cutover), not on chain yet";
  } else {
    log().info << "Holding beacon certificate generation " << generation << " (cutover at block "
               << seenCert_->cutoverHeight << "), not on chain yet: older generations' tips above it are refused";
  }
  if (const uint64_t next = getNextBlockId(); next > 0 && next - 1 > seenCert_->cutoverHeight) {
    log().warning << "This node holds blocks " << seenCert_->cutoverHeight + 1 << " to " << next - 1
                  << ", above the cutover of beacon generation " << generation
                  << ": it cannot follow that generation from here";
  }
  return CertificateOffer::Newer;
}

bool Chain::isGenerationSuperseded(uint64_t generation) const {
  return (txContext_.optAuthority.has_value() && generation < txContext_.optAuthority->beacon.generation) ||
         (seenCert_ && generation < seenCert_->generation);
}

Chain::Roe<Chain::EquivocationOffer> Chain::offerEquivocation(const Client::Equivocation &evidence) {
  if (!txContext_.optAuthority.has_value()) {
    return Error(E_STATE_INIT, "Chain authority not initialized");
  }
  if (evidence.networkId() != getNetworkId()) {
    return Error(E_TX_VALIDATION, "Equivocation evidence is for network '" + evidence.networkId() + "'");
  }
  const uint64_t generation = evidence.generation();
  if (isGenerationSuperseded(generation)) {
    return EquivocationOffer::Ignored;
  }
  if (equivocation_) {
    return *equivocation_ == evidence ? EquivocationOffer::Same : EquivocationOffer::Ignored;
  }
  const auto &inForce = txContext_.optAuthority->beacon;
  const std::string *publicKey = generation == inForce.generation ? &inForce.publicKey
                                 : seenCert_ && generation == seenCert_->generation ? &seenCert_->publicKey
                                                                                     : nullptr;
  if (!publicKey) {
    return Error(E_TX_VALIDATION, "Equivocation evidence names beacon generation " + std::to_string(generation) +
                                      ", which no certificate here names (generation " +
                                      std::to_string(inForce.generation) + " is in force)");
  }
  if (!evidence.verify(*publicKey)) {
    return Error(E_TX_SIGNATURE, "Equivocation evidence is not two different blocks at one height signed by "
                                 "the beacon of generation " + std::to_string(generation));
  }
  equivocation_ = evidence;
  log().error << "EQUIVOCATION: beacon generation " << generation << " signed two blocks at height "
              << evidence.height() << ": committing and producing nothing above it until a later generation "
              << "is certified";
  return EquivocationOffer::Newer;
}

void Chain::refreshEquivocation() {
  if (equivocation_ && isGenerationSuperseded(equivocation_->generation())) {
    log().info << "Beacon generation " << equivocation_->generation()
               << " is superseded by a later certificate: resuming past its equivocation at height "
               << equivocation_->height();
    equivocation_.reset();
  }
}

std::optional<Client::RewindReport> Chain::rewindOrder() const {
  const uint64_t next = getNextBlockId();
  if (!seenCert_ || next == 0 || next - 1 <= seenCert_->cutoverHeight) {
    return std::nullopt;
  }
  Client::RewindReport order;
  order.keptUpTo = seenCert_->cutoverHeight;
  order.droppedUpTo = next - 1;
  order.generation = seenCert_->generation;
  return order;
}

bool Chain::mayFollowTip(const Client::SignedTip &tip) const {
  if (txContext_.optAuthority) { // else nothing loaded yet: an empty node takes its first block on trust
    const uint64_t inForce = txContext_.optAuthority->beacon.generation;
    if (tip.generation < inForce) {
      return false;
    }
    // A generation no certificate here names: anyone can claim one to be followed.
    if (tip.generation > std::max(inForce, seenCert_ ? seenCert_->generation : 0)) {
      return false;
    }
  }
  if (seenCert_ && tip.generation < seenCert_->generation && tip.height > seenCert_->cutoverHeight) {
    return false;
  }
  if (equivocation_ && tip.generation == equivocation_->generation() && tip.height > equivocation_->height()) {
    return false;
  }
  return true;
}

std::vector<std::string> Chain::dialHints() const {
  std::vector<std::string> hints;
  auto add = [&hints](const BeaconCertificate &cert) {
    for (const auto &hint : cert.dialHints) {
      if (std::find(hints.begin(), hints.end(), hint) == hints.end()) {
        hints.push_back(hint);
      }
    }
  };
  if (seenCert_) {
    add(*seenCert_);
  }
  if (txContext_.optAuthority) {
    add(txContext_.optAuthority->beacon);
  }
  return hints;
}

bool Chain::cutoverForked() const {
  if (!seenCert_ || !seenCert_->hasCutover()) {
    return false;
  }
  const uint64_t cutover = seenCert_->cutoverHeight;
  if (cutover < txContext_.ledger.getStartingBlockId() || cutover >= getNextBlockId()) {
    return false;
  }
  auto block = readBlock(cutover);
  return !block || block.value().hash != seenCert_->cutoverHash;
}

std::optional<std::pair<uint64_t, std::string>> Chain::vouchedCutover() const {
  if (!seenCert_ || !seenCert_->hasCutover()) {
    return std::nullopt;
  }
  return std::make_pair(seenCert_->cutoverHeight, seenCert_->cutoverHash);
}

std::optional<Ledger::Record> Chain::certificateDueAt(uint64_t blockId) const {
  const auto vouched = vouchedCutover();
  if (!vouched || !seenCertRecord_ || blockId != vouched->first + 1 || blockId != getNextBlockId()) {
    return std::nullopt;
  }
  auto cutoverBlock = readBlock(vouched->first);
  if (!cutoverBlock || cutoverBlock.value().hash != vouched->second) {
    return std::nullopt; // another block at the cutover: halted (cutoverForked), never sealed on
  }
  return seenCertRecord_;
}

std::optional<uint64_t> Chain::rewindDepthSeconds(const Client::RewindReport &order) const {
  const uint64_t from = std::max(order.keptUpTo, txContext_.ledger.getStartingBlockId());
  auto cut = readBlock(from);
  auto last = readBlock(order.droppedUpTo);
  if (!cut || !last || last.value().block.slot < cut.value().block.slot) {
    return std::nullopt;
  }
  return (last.value().block.slot - cut.value().block.slot) * getSlotDuration();
}

std::optional<bool> Chain::rewindCrossesFinality(const Client::RewindReport &order) const {
  auto finalized = finalizedHeight();
  if (!finalized) {
    return std::nullopt;
  }
  return finalized.value().has_value() && *finalized.value() > order.keptUpTo;
}

uint64_t Chain::getFinalitySeconds() const {
  return txContext_.optChainConfig ? txContext_.optChainConfig->finalitySeconds : 0;
}

std::optional<int64_t> Chain::finalAtBelow(const Ledger::Block &block) const {
  if (block.index == 0) {
    return std::nullopt;
  }
  if (lastFinalAt_ && lastFinalAt_->first == block.previousHash) {
    return lastFinalAt_->second;
  }
  auto below = txContext_.ledger.readBlock(block.index - 1);
  if (!below) {
    return std::nullopt;
  }
  return below.value().block.finalAt;
}

void Chain::noteFinalAt(const Ledger::Block &block) {
  lastFinalAt_ = std::make_pair(calculateHash(block), block.finalAt);
}

int64_t Chain::expectedFinalAt(const Ledger::Block &block) const {
  const int64_t own = getSlotStartTime(block.slot) + static_cast<int64_t>(getFinalitySeconds());
  const auto below = finalAtBelow(block);
  return below ? std::max(own, *below) : own;
}

Chain::Roe<void> Chain::checkFinalAt(const Ledger::Block &block) const {
  const int64_t own = getSlotStartTime(block.slot) + static_cast<int64_t>(getFinalitySeconds());
  const auto below = finalAtBelow(block);
  // A node holding no block below (its first) knows only the lower bound.
  const bool known = block.index == 0 || below.has_value();
  const bool ok = known ? block.finalAt == (below ? std::max(own, *below) : own) : block.finalAt >= own;
  if (!ok) {
    return Error(E_BLOCK_HASH, "Block " + std::to_string(block.index) + " finalAt mismatch: " +
                                   std::to_string(block.finalAt) + ", expected " +
                                   std::to_string(below ? std::max(own, *below) : own));
  }
  return {};
}

Chain::Roe<std::optional<uint64_t>> Chain::finalizedHeight() const {
  const uint64_t first = txContext_.ledger.getStartingBlockId();
  const uint64_t next = getNextBlockId();
  if (next <= first) {
    return std::optional<uint64_t>{};
  }
  const int64_t now = getConsensusTimestamp();
  auto finalAt = [&](uint64_t id) -> Roe<int64_t> {
    auto b = readBlock(id);
    if (!b) {
      return b.error();
    }
    return b.value().block.finalAt;
  };
  // finalAt never falls along the chain (a running maximum, FN007): the final blocks are a prefix.
  auto lowest = finalAt(first);
  if (!lowest) {
    return lowest.error();
  }
  if (lowest.value() > now) {
    return std::optional<uint64_t>{};
  }
  uint64_t lo = first, hi = next - 1;
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo + 1) / 2;
    auto f = finalAt(mid);
    if (!f) {
      return f.error();
    }
    if (f.value() <= now) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return std::optional<uint64_t>(lo);
}

std::optional<Ledger::Record> Chain::currentCertificateRecord() const {
  if (seenCertRecord_) {
    return seenCertRecord_;
  }
  for (auto it = authoritySteps_.rbegin(); it != authoritySteps_.rend(); ++it) {
    if (it->record.type == Ledger::T_BEACON_CERT) {
      return it->record;
    }
  }
  return std::nullopt;
}

Chain::Roe<void> Chain::checkSeenCutover(const Ledger::Block &block) const {
  if (!seenCert_ || block.index <= seenCert_->cutoverHeight) {
    return {};
  }
  for (const auto &record : block.records) {
    if (record.type == Ledger::T_BEACON_CERT) {
      return {};
    }
  }
  return Error(E_BLOCK_VALIDATION, "Block " + std::to_string(block.index) + " is past block " +
                                       std::to_string(seenCert_->cutoverHeight) +
                                       ", the cutover of beacon generation " +
                                       std::to_string(seenCert_->generation) +
                                       ": it must commit a beacon certificate");
}

void Chain::refreshSeenCertificate() {
  if (!seenCert_ || !txContext_.optAuthority.has_value()) {
    return;
  }
  const auto &inForce = txContext_.optAuthority->beacon;
  if (seenCert_->generation <= inForce.generation) {
    if (seenCert_->generation == inForce.generation && seenCert_->publicKey != inForce.publicKey) {
      log().warning << "The chain committed another beacon certificate for generation " << inForce.generation
                    << " than the one seen out of band";
    } else {
      log().info << "Beacon certificate generation " << seenCert_->generation << " is on chain now";
    }
    seenCert_.reset();
    seenCertRecord_.reset();
    return;
  }
  if (auto still = verifyBeaconCertificateRecord(*seenCertRecord_); !still) {
    log().warning << "Dropping beacon certificate generation " << seenCert_->generation
                  << " seen out of band: " << still.error().message;
    seenCert_.reset();
    seenCertRecord_.reset();
  }
}

Chain::Roe<AuthorityTrail> Chain::authorityTrail() const {
  if (!txContext_.optAuthority.has_value()) {
    return Error(E_STATE_INIT, "Chain authority not initialized");
  }
  auto admin = txContext_.bank.getAccount(AccountBuffer::ID_GENESIS);
  if (!admin) {
    return Error(E_ACCOUNT_NOT_FOUND, "Genesis account not found");
  }
  AuthorityTrail trail(getNetworkId(), txContext_.optAuthority.value(), admin.value().wallet);
  trail.setSeenCertificate(seenCert_);
  trail.setEquivocation(equivocation_);
  return trail;
}

void Chain::noteAuthoritySteps(const Ledger::ChainNode &block) {
  bool changed = false;
  for (const auto &record : block.block.records) {
    if (AuthorityTrail::changesAuthority(record)) {
      authoritySteps_.push_back(Client::AuthorityStep{block.block.index, block.block.epoch, record});
      changed = true;
    }
  }
  if (changed) {
    refreshSeenCertificate();
    refreshEquivocation();
  }
}

Chain::Roe<Client::AuthorityProof> Chain::authorityProof(uint64_t upToBlockId) const {
  if (authorityStepsFrom_ != 0) {
    return Error(E_STATE_INIT, "This node holds the chain from block " + std::to_string(authorityStepsFrom_) +
                                   ", not from genesis");
  }
  if (upToBlockId >= getNextBlockId()) {
    return Error(E_BLOCK_INDEX, "Block " + std::to_string(upToBlockId) + " is beyond this node's tip");
  }
  Client::AuthorityProof proof;
  for (const auto &step : authoritySteps_) {
    if (step.blockId > upToBlockId) {
      break;
    }
    proof.steps.push_back(step);
  }
  return proof;
}

Chain::Roe<void> Chain::verifySignedTip(const Client::SignedTip &tip,
                                        const std::vector<const Ledger::Block *> &pending) const {
  auto trail = authorityTrail();
  if (!trail) {
    return trail.error();
  }
  for (const auto *block : pending) {
    if (auto observed = trail.value().observe(*block); !observed) {
      return Error(observed.error().code, observed.error().message);
    }
  }
  auto verified = trail.value().verifyTip(tip);
  if (!verified) {
    return Error(verified.error().code, verified.error().message);
  }
  return {};
}

Chain::Roe<void> Chain::verifyAccountSignatures(uint64_t accountId, const std::string &message,
                                                const std::vector<std::string> &signatures) const {
  auto account = txContext_.bank.getAccount(accountId);
  if (!account) {
    return Error(E_ACCOUNT_NOT_FOUND, "Account not found: " + std::to_string(accountId));
  }
  return verifySignaturesAgainstAccount(message, signatures, account.value());
}

Chain::Roe<void> Chain::checkAdmission(const Ledger::Record &record, int64_t nowSec,
                                      bool verifySignatures) const {
  switch (record.type) {
  case Ledger::T_GENESIS:
  case Ledger::T_RENEWAL:
  case Ledger::T_END_USER:
  case Ledger::T_IMPORT:
    // Written by a leader or a genesis, never submitted: no leader takes one from the pool.
    return Error(E_TX_VALIDATION, "A record of type " + std::to_string(record.type) + " is not submitted");
  default:
    break;
  }
  auto typed = record.decode();
  if (!typed) {
    return Error(E_TX_VALIDATION, "Unknown or malformed transaction payload");
  }
  if (verifySignatures) {
    if (auto signed_ = validateTxSignatures(record, 0, chain_block::BlockAdmissionMode::Full); !signed_) {
      return signed_;
    }
  }
  const bool ended = std::visit(
      [&](const auto &tx) {
        using T = std::decay_t<decltype(tx)>;
        if constexpr (std::is_base_of_v<Ledger::TxIdempotencyWindow, T>) {
          return tx.validationTsMax > 0 && nowSec > tx.validationTsMax;
        }
        return false;
      },
      typed.value());
  if (ended) {
    return Error(E_TX_VALIDATION, "Transaction validity window ended");
  }
  if (record.type == Ledger::T_BEACON_CERT) {
    return {}; // checked as a certificate (offerCertificate); it applies only in the block it is due
  }
  // Only what could apply takes a place in the pool: the record through the leader's own apply path
  // (fee floor, idempotency, amounts, funds, each type's rules) on the committed state. Pending
  // spends are the leader's to weigh.
  AccountBuffer scratch;
  BufferApplyContext ctx{txContext_, getNextBlockId(), getProductionSlot(), chain_block::BlockAdmissionMode::Full};
  return mapTxVoid(recordHandler_.applyBuffer(record, scratch, ctx));
}

std::optional<std::pair<uint64_t, uint64_t>> Chain::idempotencyKeyOf(const Ledger::Record &record) const {
  auto key = recordHandler_.getIdempotencyKey(record);
  return key ? key.value() : std::nullopt;
}

Chain::Roe<void> Chain::validateTxSignatures(
    const Ledger::Record &record,
    uint64_t slotLeaderId,
    chain_block::BlockAdmissionMode admissionMode,
    const AccountBuffer *working) const {
  // Keys as the state this record applies to holds them (the seal applies on the tip bank, in order).
  auto bankHolding = [&](uint64_t id) -> const AccountBuffer & {
    return working && working->hasAccount(id) ? *working : txContext_.bank;
  };
  if (record.type == Ledger::T_IMPORT) {
    // Unsigned: the predecessor's state root authenticates the import (B032).
    return record.signatures.empty() ? Roe<void>() : Error(E_TX_SIGNATURE, "An import is unsigned");
  }
  if (record.signatures.size() < 1) {
    return Error(E_TX_SIGNATURE,
                 "Transaction must have at least one signature");
  }

  if (record.type == Ledger::T_RECOVERY) {
    // Signed by the recovery set in the chain authority, not by an account (ba7).
    if (!txContext_.optAuthority.has_value()) {
      return chain_block::admissionTxStrict(admissionMode)
                 ? Error(E_STATE_INIT, "Chain authority not initialized")
                 : Roe<void>();
    }
    return mapTxVoid(chain_tx::verifyRecoverySigned(record, txContext_.optAuthority->recovery, getNetworkId(),
                                                    txContext_.crypto, log()));
  }
  if (record.type == Ledger::T_ACCOUNT_RECOVERY) {
    // Signed by the account's recovery set, not its keys (projects/account-recovery).
    auto tx = utl::binaryUnpack<Ledger::TxAccountRecovery>(record.data);
    if (!tx) {
      return Error(E_TX_VALIDATION, "Malformed account recovery");
    }
    auto account = bankHolding(tx.value().walletId).getAccount(tx.value().walletId);
    if (!account) {
      return chain_block::admissionTxStrict(admissionMode)
                 ? Error(E_ACCOUNT_NOT_FOUND, "Account not found: " + std::to_string(tx.value().walletId))
                 : Roe<void>();
    }
    const Client::AccountRecovery &recovery = account.value().wallet.recovery;
    if (!recovery.hasSet()) {
      return Error(E_TX_SIGNATURE, "Account " + std::to_string(tx.value().walletId) + " has no recovery set");
    }
    AccountBuffer::Account signers = account.value();
    signers.wallet.publicKeys = recovery.publicKeys;
    signers.wallet.minSignatures = recovery.minSignatures;
    return verifySignaturesAgainstAccount(record.signingMessage(getNetworkId()), record.signatures, signers);
  }
  auto signerAccountIdRoe =
      recordHandler_.getSignerAccountId(record, slotLeaderId);
  if (!signerAccountIdRoe) {
    return Error(signerAccountIdRoe.error());
  }
  const uint64_t signerAccountId = signerAccountIdRoe.value();

  auto accountResult = bankHolding(signerAccountId).getAccount(signerAccountId);
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
