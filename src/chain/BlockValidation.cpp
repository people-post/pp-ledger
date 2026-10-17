#include "BlockValidation.h"
#include "MinerLiveness.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "common/Serialize.hpp"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Utilities.h"
#include "../consensus/EpochSeed.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace pp::chain_block {

namespace {

/** Future-slot slack for arriving blocks: max(2 slots, 15 s) of clock skew. */
constexpr uint64_t kFutureSlotToleranceSlots = 2;
constexpr uint64_t kFutureSlotToleranceSeconds = 15;

/** Unpack a genesis bootstrap T_NEW_USER record and require exact min fee. */
chain_tx::Roe<Ledger::TxNewUser> loadGenesisNewUserWithExactFee(
    const Ledger::Record &rec, const BlockChainConfig &config,
    const RecordHandler &recordHandler, const char *wrongTypeMsg,
    const char *deserializeFailMsg, const char *feeMismatchPrefix) {
  if (rec.type != Ledger::T_NEW_USER) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, wrongTypeMsg);
  }
  auto txRoe = utl::binaryUnpack<Ledger::TxNewUser>(rec.data);
  if (!txRoe) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, deserializeFailMsg);
  }
  const auto &tx = txRoe.value();
  const Ledger::TypedTx typedTx(tx);
  auto feeResult = chain_tx::calculateMinimumFeeForTransaction(
      config, typedTx,
      [&recordHandler](const BlockChainConfig &cfg, const Ledger::TypedTx &t) {
        return recordHandler.getBillableCustomMetaSizeForFee(cfg, t);
      });
  if (!feeResult) {
    return feeResult.error();
  }
  if (tx.fee != feeResult.value()) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             std::string(feeMismatchPrefix) +
                                 std::to_string(feeResult.value()));
  }
  return tx;
}


chain_tx::Roe<void> requireGenesisBootstrapRouting(
    const Ledger::TxNewUser &tx, uint64_t expectedTo, const char *routingMsg) {
  if (tx.fromWalletId != AccountBuffer::ID_GENESIS ||
      tx.toWalletId != expectedTo) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, routingMsg);
  }
  return {};
}

chain_tx::Roe<int64_t> requireFeeFitsInt64(uint64_t fee,
                                          const char *overflowMsg) {
  if (fee > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, overflowMsg);
  }
  return static_cast<int64_t>(fee);
}

/** Add two int64_t values, returning false (instead of wrapping) on overflow. */
bool safeAddI64(int64_t a, int64_t b, int64_t &out) {
  if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<int64_t>::min() - b)) {
    return false;
  }
  out = a + b;
  return true;
}

/**
 * System account records after the config record. Fee is created first: the
 * others pay their fees into it. Only reserve is funded (the rest of supply).
 */
struct GenesisSystemRecord {
  uint64_t id;
  bool funded;
  const char *name;
};
constexpr GenesisSystemRecord kGenesisSystemRecords[] = {
    {AccountBuffer::ID_FEE, false, "fee"},
    {AccountBuffer::ID_RESERVE, true, "reserve"},
    {AccountBuffer::ID_REGISTRAR, false, "registrar"},
    {AccountBuffer::ID_RECYCLE, false, "recycle"},
};

/** Add a record's amount and fee to `total`, refusing int64 overflow. */
chain_tx::Roe<void> addGenesisSpend(const Ledger::TxNewUser &tx, int64_t &total) {
  auto amount = requireFeeFitsInt64(tx.amount, "Genesis transaction amount exceeds int64_t range");
  if (!amount) {
    return amount.error();
  }
  auto fee = requireFeeFitsInt64(tx.fee, "Genesis transaction fee exceeds int64_t range");
  if (!fee) {
    return fee.error();
  }
  if (!safeAddI64(total, amount.value(), total) || !safeAddI64(total, fee.value(), total)) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Genesis transactions amount/fee sum overflowed");
  }
  return {};
}

chain_tx::Roe<void> validateGenesisSystemRecord(const Ledger::TxNewUser &tx, const GenesisSystemRecord &expected) {
  const std::string name = expected.name;
  if (auto routing = requireGenesisBootstrapRouting(
          tx, expected.id, ("Genesis " + name + " account must be created from genesis at its id").c_str());
      !routing) {
    return routing;
  }
  if (tx.meta.empty()) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Genesis " + name + " account creation must have meta");
  }
  if (!expected.funded && tx.amount != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Genesis " + name + " account must start with amount 0");
  }
  return {};
}

chain_tx::Roe<void> validateGenesisMinerRecord(const Ledger::TxNewUser &tx, std::set<uint64_t> &seen) {
  if (tx.fromWalletId != AccountBuffer::ID_GENESIS || tx.toWalletId < AccountBuffer::ID_FIRST_ISSUED ||
      tx.toWalletId >= AccountBuffer::ID_FIRST_USER) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis miner must be funded from genesis into the issued id range");
  }
  if (!seen.insert(tx.toWalletId).second) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Duplicate genesis miner account");
  }
  if (tx.amount == 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Genesis miner must have a positive stake");
  }
  return {};
}

/**
 * Check the system and genesis miner records (records 1..n) and return what
 * they spend from genesis in total (amounts + fees).
 */
chain_tx::Roe<int64_t> validateGenesisAccountRecords(const Ledger::ChainNode &block,
                                                     const BlockChainConfig &config,
                                                     const RecordHandler &recordHandler) {
  int64_t total = 0;
  const auto &records = block.block.records;
  std::set<uint64_t> minerIds;
  for (size_t i = 1; i < records.size(); ++i) {
    auto txRoe = loadGenesisNewUserWithExactFee(
        records[i], config, recordHandler, "Genesis account record must be a new user transaction",
        "Failed to deserialize genesis account tx payload", "Genesis account creation must have fee: ");
    if (!txRoe) {
      return txRoe.error();
    }
    const auto &tx = txRoe.value();
    auto shape = i <= std::size(kGenesisSystemRecords) ? validateGenesisSystemRecord(tx, kGenesisSystemRecords[i - 1])
                                                       : validateGenesisMinerRecord(tx, minerIds);
    if (!shape) {
      return shape.error();
    }
    if (auto spent = addGenesisSpend(tx, total); !spent) {
      return spent.error();
    }
  }
  return total;
}

} // namespace

std::string calculateBlockHash(const Ledger::Block &block) {
  return utl::sha256Raw(block.headerToString());
}

std::string calculateTxRoot(const std::vector<Ledger::Record> &records) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  for (const auto &rec : records) {
    std::string packed = utl::binaryPack(rec);
    ar & packed;
  }
  return utl::sha256Raw(std::string("pp-ledger/txroot/v1") + oss.str());
}

std::string calculateStakeSnapshotHash(
    const std::vector<consensus::Stakeholder> &stakeholders) {
  std::vector<consensus::Stakeholder> sorted = stakeholders;
  std::sort(sorted.begin(), sorted.end(),
            [](const consensus::Stakeholder &a,
               const consensus::Stakeholder &b) { return a.id < b.id; });
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  for (const auto &s : sorted) {
    ar & s.id & s.stake;
  }
  return utl::sha256Raw(std::string("pp-ledger/stake/v1") + oss.str());
}

std::string calculateGenesisConfigDigest(const BlockChainConfig &config) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & config;
  return utl::sha256Raw(std::string("pp-ledger/epoch-seed/genesis-config/v1") +
                        oss.str());
}

chain_tx::Roe<void> validatePredecessorHeader(const ChainPredecessor &predecessor) {
  const auto &header = predecessor.header;
  if (header.index != predecessor.height || calculateBlockHash(header) != predecessor.blockHash ||
      header.stateRoot != predecessor.stateRoot) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "The predecessor's header is not block " + std::to_string(predecessor.height) +
                                 " with that hash and state root");
  }
  return {};
}

chain_tx::Roe<void> validateImportBlockBody(const Ledger::ChainNode &block, const Ledger &ledger,
                                            const BlockChainConfig &config, uint64_t remaining) {
  const auto refused = [&](const std::string &why) {
    return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION,
                             "Import block " + std::to_string(block.block.index) + ": " + why);
  };
  const auto &b = block.block;
  if (b.slot != 0 || b.slotLeader != 0 || b.epoch != 0) {
    return refused("must be at slot 0, epoch 0, with no leader");
  }
  if (b.timestamp != config.genesisTime) {
    return refused("must be stamped at genesis time");
  }
  if (b.stakeSnapshotHash != calculateStakeSnapshotHash({}) || !b.committee.empty()) {
    return refused("elects no committee (the empty stake snapshot)");
  }
  auto genesis = ledger.readBlock(0);
  if (!genesis || b.epochSeed != genesis.value().block.epochSeed) {
    return refused("must carry the genesis epoch seed");
  }
  if (b.records.empty() || b.records.size() > remaining) {
    return refused("holds from 1 to the " + std::to_string(remaining) + " accounts still to import");
  }
  for (const auto &record : b.records) {
    if (record.type != Ledger::T_IMPORT) {
      return refused("holds imports only");
    }
  }
  if (config.maxBlockBytes > 0 && b.ltsToString().size() > config.maxBlockBytes) {
    return refused("more than maxBlockBytes");
  }
  return {};
}

chain_tx::Roe<void> validateGenesisBlock(const Ledger::ChainNode &block,
                                        const RecordHandler &recordHandler) {
  if (block.block.index != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have index 0");
  }
  if (block.block.previousHash != utl::zeroHash()) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have zero previousHash");
  }
  if (block.block.slot != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have slot 0");
  }
  if (block.block.slotLeader != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have slotLeader 0");
  }
  if (block.block.epoch != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have epoch 0");
  }
  if (block.block.txIndex != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have txIndex 0");
  }
  const std::string expectedStakeHash =
      calculateStakeSnapshotHash({});
  if (block.block.stakeSnapshotHash != expectedStakeHash) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis block must commit empty stake snapshot");
  }
  if (block.block.epochSeed.size() != utl::SHA256_DIGEST_SIZE) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must commit 32-byte epochSeed");
  }
  if (block.block.records.empty()) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Genesis block must have the genesis transaction");
  }

  const std::string expectedTxRoot =
      calculateTxRoot(block.block.records);
  if (block.block.txRoot != expectedTxRoot) {
    return chain_tx::TxError(chain_err::E_BLOCK_HASH,
                             "Genesis block txRoot mismatch");
  }
  if (!block.block.committee.empty()) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Genesis block elects no committee");
  }

  const auto &checkpointRec = block.block.records[0];
  if (checkpointRec.type != Ledger::T_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "First genesis transaction must be genesis transaction");
  }
  auto checkpointTxRoe = utl::binaryUnpack<Ledger::TxGenesis>(checkpointRec.data);
  if (!checkpointTxRoe) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Failed to deserialize genesis tx payload");
  }
  const auto &checkpointTx = checkpointTxRoe.value();
  GenesisAccountMeta gm;
  if (!gm.ltsFromString(checkpointTx.meta)) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Failed to deserialize genesis checkpoint meta");
  }
  {
    const std::string cfgDigest = calculateGenesisConfigDigest(gm.config);
    const std::string expectedSeed = consensus::deriveGenesisEpochSeed(
        gm.config.networkId, cfgDigest);
    if (block.block.epochSeed != expectedSeed) {
      return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                               "Genesis block epochSeed mismatch");
    }
  }

  if (gm.hasImport) {
    // An importing genesis (B032): the accounts come in the import blocks after it.
    if (block.block.records.size() != 1) {
      return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                               "An importing genesis block holds the genesis transaction only");
    }
    std::string calculatedHash = calculateBlockHash(block.block);
    if (calculatedHash != block.hash) {
      return chain_tx::TxError(chain_err::E_BLOCK_HASH, "Genesis block hash validation failed");
    }
    return {};
  }
  // Config, then the system accounts in kGenesisSystemRecords order, then any
  // genesis miner accounts.
  if (block.block.records.size() < 1 + std::size(kGenesisSystemRecords)) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis block must have the config and system account transactions");
  }
  auto supplyRoe = validateGenesisAccountRecords(block, gm.config, recordHandler);
  if (!supplyRoe) {
    return supplyRoe.error();
  }
  if (supplyRoe.value() != static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY)) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis account transactions must spend exactly the initial supply (amounts + fees): " +
            std::to_string(AccountBuffer::INITIAL_TOKEN_SUPPLY));
  }

  std::string calculatedHash = calculateBlockHash(block.block);
  if (calculatedHash != block.hash) {
    return chain_tx::TxError(chain_err::E_BLOCK_HASH,
                             "Genesis block hash validation failed");
  }
  return {};
}

chain_tx::Roe<void> validateBlockSequence(const Ledger &ledger,
                                          const Ledger::ChainNode &block) {
  // NOTE: this structural check is shared by live tip admission, sealed-block
  // commit verification, and full-history replay from disk (loadFromLedger).
  // During replay, ledger.getNextBlockId() already reflects the final,
  // fully-persisted block count -- not "the next index to admit" -- so it
  // cannot be used here to reject index < nextBlockId. That stricter
  // live-admission-only invariant (a new block's index must equal the
  // ledger's current next-block-id) is enforced by the caller in
  // Chain::addBlock instead; Ledger::addBlock enforces it again at persist
  // time as a final backstop.
  const uint64_t startingBlockId = ledger.getStartingBlockId();
  if (block.block.index < startingBlockId) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_INDEX,
        "Invalid block index: expected >= " + std::to_string(startingBlockId) +
            " got " + std::to_string(block.block.index));
  }

  const uint64_t nextBlockId = ledger.getNextBlockId();
  if (block.block.index > nextBlockId) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_INDEX,
        "Invalid block index: expected <= " + std::to_string(nextBlockId) +
            " got " + std::to_string(block.block.index));
  }

  if (block.block.index > startingBlockId) {
    auto prevBlockResult = ledger.readBlock(block.block.index - 1);
    if (!prevBlockResult) {
      return chain_tx::TxError(
          chain_err::E_BLOCK_NOT_FOUND,
          "Latest block not found: " + std::to_string(block.block.index - 1));
    }
    auto prevBlock = prevBlockResult.value();

    if (block.block.index != prevBlock.block.index + 1) {
      return chain_tx::TxError(
          chain_err::E_BLOCK_INDEX,
          "Invalid block index: expected " +
              std::to_string(prevBlock.block.index + 1) + " got " +
              std::to_string(block.block.index));
    }

    if (block.block.previousHash != prevBlock.hash) {
      return chain_tx::TxError(
          chain_err::E_BLOCK_HASH,
          "Invalid previous hash: expected " + prevBlock.hash + " got " +
              block.block.previousHash);
    }

    const uint64_t expectedTxIndex =
        prevBlock.block.txIndex + prevBlock.block.records.size();
    if (block.block.txIndex != expectedTxIndex) {
      return chain_tx::TxError(
          chain_err::E_BLOCK_INDEX,
          "Invalid txIndex: expected " + std::to_string(expectedTxIndex) +
              " got " + std::to_string(block.block.txIndex));
    }
  }

  return {};
}

chain_tx::Roe<void>
validateIntraBlockIdempotency(const Ledger::ChainNode &block,
                               const RecordHandler &recordHandler) {
  std::set<std::pair<uint64_t, uint64_t>> seenIdempotentPairs;
  for (const auto &rec : block.block.records) {
    auto typedRoe = rec.decode();
    if (!typedRoe) {
      continue;
    }
    if (rec.type >= RecordHandler::kNumTxTypes) {
      continue;
    }
    const ITxHandler *handler = recordHandler.get(rec.type);
    if (!handler) {
      continue;
    }
    auto keyRoe = handler->getIdempotencyKey(typedRoe.value());
    if (!keyRoe) {
      return keyRoe.error();
    }
    const auto &optKey = keyRoe.value();
    if (!optKey.has_value()) {
      continue;
    }
    auto key = optKey.value();
    if (!seenIdempotentPairs.insert(key).second) {
      return chain_tx::TxError(
          chain_err::E_TX_IDEMPOTENCY,
          "Duplicate idempotent id within block: " +
              std::to_string(key.second) +
              " for wallet: " + std::to_string(key.first));
    }
  }
  return {};
}

std::vector<Ledger::CommitteeMember> toCommittee(const std::vector<consensus::Stakeholder> &stakeholders) {
  std::vector<Ledger::CommitteeMember> committee;
  committee.reserve(stakeholders.size());
  for (const auto &s : stakeholders) {
    committee.push_back(Ledger::CommitteeMember{s.id, s.stake});
  }
  std::sort(committee.begin(), committee.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  return committee;
}

std::vector<consensus::Stakeholder> fromCommittee(const std::vector<Ledger::CommitteeMember> &committee) {
  std::vector<consensus::Stakeholder> stakeholders;
  stakeholders.reserve(committee.size());
  for (const auto &m : committee) {
    stakeholders.push_back(consensus::Stakeholder{m.id, m.stake});
  }
  return stakeholders;
}

bool opensEpoch(const Ledger &ledger, const Ledger::Block &block) {
  if (block.index == 0) {
    return true;
  }
  auto previous = ledger.readBlock(block.index - 1);
  return !previous || previous.value().block.epoch < block.epoch;
}

chain_tx::Roe<void> validateBlockCommittee(const Ledger &ledger, const Ledger::Block &block) {
  if (!opensEpoch(ledger, block)) {
    if (!block.committee.empty()) {
      return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION,
                               "Block " + std::to_string(block.index) + " does not open its epoch: no committee");
    }
    return {};
  }
  if (!std::is_sorted(block.committee.begin(), block.committee.end(),
                      [](const auto &a, const auto &b) { return a.id < b.id; }) ||
      calculateStakeSnapshotHash(fromCommittee(block.committee)) != block.stakeSnapshotHash) {
    return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION,
                             "Block " + std::to_string(block.index) +
                                 " opens epoch " + std::to_string(block.epoch) +
                                 ": its committee does not match stakeSnapshotHash");
  }
  return {};
}

bool isGenesisRenewal(const Ledger::Record &record) {
  if (record.type != Ledger::T_RENEWAL) {
    return false;
  }
  auto tx = utl::binaryUnpack<Ledger::TxRenewal>(record.data);
  return tx && tx.value().walletId == AccountBuffer::ID_GENESIS;
}

bool rotatesCheckpoint(const Ledger &ledger, const consensus::SlotCommittee &consensus,
                       const BlockChainConfig &config, const Checkpoint &checkpoint,
                       const Ledger::ChainNode &block) {
  return !block.block.records.empty() && isGenesisRenewal(block.block.records.front()) &&
         checkpointEligible(ledger, consensus, config, checkpoint, block.block.index, block.block.epoch);
}

bool renewalsWaitBelow(const AccountBuffer &bank, uint64_t blockId) {
  return blockId > 0 && !bank.getAccountIdsBeforeBlockId(blockId).empty();
}

bool checkpointEligible(const Ledger &ledger, const consensus::SlotCommittee &consensus,
                        const BlockChainConfig &config, const Checkpoint &checkpoint, uint64_t index,
                        uint64_t epoch) {
  if (index == 0 || index <= checkpoint.currentId || index < checkpoint.currentId + config.checkpoint.minBlocks) {
    return false;
  }
  auto previous = ledger.readBlock(index - 1);
  if (!previous || previous.value().block.epoch >= epoch) {
    return false; // not the first block of its epoch (or the block before is not held)
  }
  auto current = ledger.readBlock(checkpoint.currentId);
  if (!current) {
    return false;
  }
  // Strictly older than the renewal cut-off at the previous block: every account last written
  // before the current checkpoint was due there (calculateMaxBlockIdForRenewal).
  const int64_t previousSlotStart = consensus.getSlotStartTime(previous.value().block.slot);
  return current.value().block.timestamp + static_cast<int64_t>(config.checkpoint.minAgeSeconds) <
         previousSlotStart;
}

chain_tx::Roe<uint64_t> calculateMaxBlockIdForRenewal(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, uint64_t atBlockId, uint64_t atSlot) {
  if (!optChainConfig.has_value()) {
    if (checkpoint.currentId > checkpoint.lastId) {
      return chain_tx::TxError(
          chain_err::E_INTERNAL,
          "Chain config not initialized; expected config when "
          "checkpoint.currentId > checkpoint.lastId");
    }
    return uint64_t{0};
  }
  const BlockChainConfig &config = optChainConfig.value();
  const uint64_t minBlocks = config.checkpoint.minBlocks;
  if (atBlockId < minBlocks) {
    return uint64_t{0};
  }
  uint64_t maxBlockIdFromBlocks = atBlockId - minBlocks + 1;

  const uint64_t minAgeSeconds = config.checkpoint.minAgeSeconds;

  uint64_t maxBlockIdFromTime = atBlockId;
  if (minAgeSeconds > 0 && atBlockId > 0) {
    // The block's own slot, not this node's clock: every node, and a replay years later, agrees.
    const int64_t cutoffTimestamp =
        consensus.getSlotStartTime(atSlot) - static_cast<int64_t>(minAgeSeconds);
    auto roeBlock = ledger.findBlockByTimestamp(cutoffTimestamp);
    if (roeBlock) {
      maxBlockIdFromTime = roeBlock.value().block.index;
    }
  }
  const uint64_t maxBlockIdForRenewal =
      std::min(maxBlockIdFromBlocks, maxBlockIdFromTime);
  if (maxBlockIdForRenewal == 0 || maxBlockIdForRenewal >= atBlockId) {
    return uint64_t{0};
  }
  return maxBlockIdForRenewal;
}

chain_tx::Roe<void> validateAccountRenewals(
    const Ledger::ChainNode &block, const AccountBuffer &bank,
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, const RecordHandler &recordHandler) {
  auto maxBlockIdResult = calculateMaxBlockIdForRenewal(
      ledger, consensus, optChainConfig, checkpoint, block.block.index, block.block.slot);
  if (!maxBlockIdResult) {
    return chain_tx::Roe<void>(maxBlockIdResult.error());
  }
  const uint64_t maxBlockIdForRenewal = maxBlockIdResult.value();

  // A checkpoint is an eligible block that opens with the genesis renewal, due or not: a node
  // starting there reads config and authority from it (beacon-authority ba5b). It must be ready:
  // every account last written before the current checkpoint renewed (B035). At an eligible block
  // that is not one, the genesis renewal waits (it may not appear elsewhere in the block).
  const bool eligible =
      optChainConfig.has_value() &&
      checkpointEligible(ledger, consensus, optChainConfig.value(), checkpoint, block.block.index, block.block.epoch);
  const bool checkpointBlock =
      eligible && !block.block.records.empty() && isGenesisRenewal(block.block.records.front());
  if (checkpointBlock && renewalsWaitBelow(bank, checkpoint.currentId)) {
    return chain_tx::TxError(chain_err::E_ACCOUNT_RENEWAL,
                             "Checkpoint block " + std::to_string(block.block.index) +
                                 ": accounts last written before checkpoint " +
                                 std::to_string(checkpoint.currentId) + " still wait for their renewal");
  }
  if (eligible && !checkpointBlock &&
      std::any_of(block.block.records.begin(), block.block.records.end(), isGenesisRenewal)) {
    return chain_tx::TxError(chain_err::E_ACCOUNT_RENEWAL,
                             "Block " + std::to_string(block.block.index) +
                                 " may be a checkpoint: the genesis renewal opens it or waits");
  }

  std::set<uint64_t> accountsNeedingRenewal;
  if (maxBlockIdForRenewal > 0) {
    for (uint64_t accountId :
         bank.getAccountIdsBeforeBlockId(maxBlockIdForRenewal)) {
      accountsNeedingRenewal.insert(accountId);
    }
  }

  std::set<uint64_t> accountsRenewedInBlock;

  for (const auto &rec : block.block.records) {
    if (rec.type >= RecordHandler::kNumTxTypes) {
      continue;
    }
    const ITxHandler *handler = recordHandler.get(rec.type);
    if (!handler) {
      continue;
    }
    auto typedRoe = rec.decode();
    if (!typedRoe) {
      if (handler->participatesInAccountRenewalValidation()) {
        return chain_tx::TxError(chain_err::E_ACCOUNT_RENEWAL,
                                 "Failed to deserialize renewal-related tx payload");
      }
      continue;
    }
    auto accountIdRoe = handler->getRenewalAccountIdIfAny(typedRoe.value());
    if (!accountIdRoe) {
      return accountIdRoe.error();
    }
    if (!accountIdRoe.value().has_value()) {
      continue;
    }
    const uint64_t accountId = accountIdRoe.value().value();

    auto accountResult = bank.getAccount(accountId);
    if (!accountResult) {
      return chain_tx::TxError(
          chain_err::E_ACCOUNT_RENEWAL,
          "Account not found in renewal transaction: " +
              std::to_string(accountId));
    }
    const auto &account = accountResult.value();

    const bool checkpointGenesisRenewal = checkpointBlock && accountId == AccountBuffer::ID_GENESIS;
    if (maxBlockIdForRenewal > 0 && account.blockId > maxBlockIdForRenewal && !checkpointGenesisRenewal) {
      return chain_tx::TxError(
          chain_err::E_ACCOUNT_RENEWAL,
          "Account renewal too early: account " + std::to_string(accountId) +
              " has blockId " + std::to_string(account.blockId) +
              " but deadline is at blockId " +
              std::to_string(maxBlockIdForRenewal));
    }

    accountsRenewedInBlock.insert(accountId);
  }

  // Due renewals may wait when they do not fit (B035): a block leaving any out holds renewals only
  // (and at most a beacon certificate, which a takeover's block needs), at least one.
  for (uint64_t accountId : accountsNeedingRenewal) {
    if (accountsRenewedInBlock.find(accountId) != accountsRenewedInBlock.end()) {
      continue;
    }
    for (const auto &rec : block.block.records) {
      const ITxHandler *handler = rec.type < RecordHandler::kNumTxTypes ? recordHandler.get(rec.type) : nullptr;
      const bool renewal = handler != nullptr && handler->participatesInAccountRenewalValidation();
      if (!renewal && rec.type != Ledger::T_BEACON_CERT) {
        return chain_tx::TxError(chain_err::E_ACCOUNT_RENEWAL,
                                 "Missing required account renewal: account " + std::to_string(accountId) +
                                     " meets renewal deadline, and a block leaving due renewals out holds "
                                     "renewals only");
      }
    }
    if (accountsRenewedInBlock.empty()) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_RENEWAL,
                               "Missing required account renewal: account " + std::to_string(accountId) +
                                   " meets renewal deadline, and the block renews none");
    }
    break; // the block is renewals only: the rest waits too
  }

  return {};
}

chain_tx::Roe<void> validateEmptyHeartbeatPolicy(const Ledger::ChainNode &block,
                                                 uint64_t tipSlot,
                                                 const BlockChainConfig &config,
                                                 bool producerDue) {
  if (!block.block.records.empty()) {
    return {};
  }
  if (producerDue ||
      shouldSealEmptyHeartbeat(block.block.slot, tipSlot,
                               config.heartbeatSlots)) {
    return {};
  }
  if (config.heartbeatSlots == 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION,
                             "Empty block refused: producer not due and heartbeatSlots=0");
  }
  return chain_tx::TxError(
      chain_err::E_BLOCK_VALIDATION,
      "Empty heartbeat premature: slot lag below heartbeatSlots");
}

chain_tx::Roe<void> validateAuthorityRecordCount(const Ledger::Block &block) {
  size_t certificates = 0;
  size_t recoveries = 0;
  for (const auto &record : block.records) {
    certificates += record.type == Ledger::T_BEACON_CERT ? 1 : 0;
    recoveries += record.type == Ledger::T_RECOVERY ? 1 : 0;
  }
  if (certificates > 1) {
    return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION, "A block holds at most one beacon certificate");
  }
  if (recoveries > 1) {
    return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION, "A block holds at most one recovery announcement");
  }
  return {};
}

chain_tx::Roe<void> checkBlockStructural(const Ledger::ChainNode &block,
                                         const Ledger &ledger) {
  const std::string expectedTxRoot = calculateTxRoot(block.block.records);
  if (block.block.txRoot != expectedTxRoot) {
    return chain_tx::TxError(chain_err::E_BLOCK_HASH, "Block txRoot mismatch");
  }

  std::string calculatedHash = calculateBlockHash(block.block);
  if (calculatedHash != block.hash) {
    return chain_tx::TxError(chain_err::E_BLOCK_HASH,
                             "Block hash validation failed");
  }
  if (auto committee = validateBlockCommittee(ledger, block.block); !committee) {
    return committee;
  }
  if (auto authority = validateAuthorityRecordCount(block.block); !authority) {
    return authority;
  }

  return validateBlockSequence(ledger, block);
}

chain_tx::Roe<void>
checkBlockConsensus(const Ledger::ChainNode &block,
                    const consensus::SlotCommittee &consensus) {
  const uint64_t slot = block.block.slot;
  const uint64_t slotLeader = block.block.slotLeader;
  const uint64_t expectedEpoch = consensus.getEpochFromSlot(slot);
  if (block.block.epoch != expectedEpoch) {
    return chain_tx::TxError(
        chain_err::E_CONSENSUS_SLOT_LEADER,
        "Block epoch mismatch: expected " + std::to_string(expectedEpoch) +
            " got " + std::to_string(block.block.epoch));
  }
  const std::string expectedStakeHash =
      calculateStakeSnapshotHash(consensus.getStakeholders());
  if (block.block.stakeSnapshotHash != expectedStakeHash) {
    return chain_tx::TxError(chain_err::E_CONSENSUS_SLOT_LEADER,
                             "Block stakeSnapshotHash mismatch");
  }
  if (block.block.epochSeed.size() != utl::SHA256_DIGEST_SIZE) {
    return chain_tx::TxError(chain_err::E_CONSENSUS_SLOT_LEADER,
                             "Block epochSeed must be 32 bytes");
  }
  if (!consensus.hasEpochSeedFor(expectedEpoch)) {
    return chain_tx::TxError(chain_err::E_CONSENSUS_SLOT_LEADER,
                             "Consensus epoch seed not installed for block epoch");
  }
  if (block.block.epochSeed != consensus.getEpochSeed()) {
    return chain_tx::TxError(chain_err::E_CONSENSUS_SLOT_LEADER,
                             "Block epochSeed mismatch");
  }
  if (!consensus.validateSlotLeader(slotLeader, slot)) {
    return chain_tx::TxError(
        chain_err::E_CONSENSUS_SLOT_LEADER,
        "Invalid slot leader for block at slot " + std::to_string(slot));
  }
  if (!consensus.validateBlockTiming(block.block.timestamp, slot)) {
    return chain_tx::TxError(chain_err::E_CONSENSUS_TIMING,
                             "Block timestamp outside valid slot range");
  }
  return {};
}

uint64_t futureSlotToleranceSlots(uint64_t slotDuration) {
  if (slotDuration == 0) {
    return kFutureSlotToleranceSlots;
  }
  const uint64_t bySeconds =
      (kFutureSlotToleranceSeconds + slotDuration - 1) / slotDuration;
  return std::max(kFutureSlotToleranceSlots, bySeconds);
}

chain_tx::Roe<void>
checkBlockArrival(const Ledger::ChainNode &block, const Ledger &ledger,
                  const consensus::SlotCommittee &consensus) {
  const uint64_t slot = block.block.slot;
  if (block.block.index > ledger.getStartingBlockId()) {
    auto prevBlockResult = ledger.readBlock(block.block.index - 1);
    if (!prevBlockResult) {
      return chain_tx::TxError(
          chain_err::E_BLOCK_NOT_FOUND,
          "Latest block not found: " + std::to_string(block.block.index - 1));
    }
    const uint64_t prevSlot = prevBlockResult.value().block.slot;
    if (slot <= prevSlot) {
      return chain_tx::TxError(chain_err::E_BLOCK_SEQUENCE,
                               "Invalid block slot: expected > " +
                                   std::to_string(prevSlot) + " got " +
                                   std::to_string(slot));
    }
  }

  const uint64_t currentSlot = consensus.getCurrentSlot();
  const uint64_t tolerance =
      futureSlotToleranceSlots(consensus.getConfig().slotDuration);
  if (slot > currentSlot + tolerance) {
    return chain_tx::TxError(
        chain_err::E_CONSENSUS_TIMING,
        "Block slot " + std::to_string(slot) +
            " is too far in the future (current slot " +
            std::to_string(currentSlot) + ")");
  }
  return {};
}

chain_tx::Roe<void> checkBlockBodyPolicy(
    const Ledger::ChainNode &block, const AccountBuffer &bank,
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, const RecordHandler &recordHandler) {
  auto renewalValidation = validateAccountRenewals(
      block, bank, ledger, consensus, optChainConfig, checkpoint, recordHandler);
  if (!renewalValidation) {
    return renewalValidation;
  }

  if (!optChainConfig.has_value()) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Chain config not initialized; expected config for body policy");
  }
  // Both limits bind every record, renewals included: due renewals that do not fit wait (B035).
  const uint64_t maxTx = optChainConfig.value().maxTransactionsPerBlock;
  if (maxTx > 0 && block.block.records.size() > maxTx) {
    return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION,
                             "Block has more than max transactions per block (" +
                                 std::to_string(block.block.records.size()) + " > " + std::to_string(maxTx) + ")");
  }
  const uint64_t maxBytes = optChainConfig.value().maxBlockBytes;
  if (maxBytes > 0) {
    const uint64_t bytes = block.block.ltsToString().size();
    if (bytes > maxBytes) {
      return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION, "Block takes " + std::to_string(bytes) +
                                                                  " bytes, more than maxBlockBytes (" +
                                                                  std::to_string(maxBytes) + ")");
    }
  }
  if (block.block.records.empty()) {
    auto prevRoe = ledger.readBlock(block.block.index - 1);
    if (!prevRoe) {
      return chain_tx::TxError(
          chain_err::E_BLOCK_NOT_FOUND,
          "Previous block not found for empty heartbeat check");
    }
    // An empty body leaves the bank at its pre-block state: the producer's miner clock is the
    // one this block would answer.
    auto producer = bank.getAccount(block.block.slotLeader);
    const bool producerDue =
        producer && chain_tx::isMinerDue(producer.value().wallet.miner,
                                         block.block.slot,
                                         optChainConfig.value());
    auto heartbeat = validateEmptyHeartbeatPolicy(
        block, prevRoe->block.slot, optChainConfig.value(), producerDue);
    if (!heartbeat) {
      return heartbeat;
    }
  }
  return validateIntraBlockIdempotency(block, recordHandler);
}

chain_tx::Roe<void>
checkBlock(const Ledger::ChainNode &block, BlockAdmissionMode mode,
           const Ledger &ledger, const consensus::SlotCommittee &consensus,
           const AccountBuffer &bank,
           const std::optional<BlockChainConfig> &optChainConfig,
           const Checkpoint &checkpoint, const RecordHandler &recordHandler) {
  auto structural = checkBlockStructural(block, ledger);
  if (!structural) {
    return structural;
  }
  if (!admissionRunsConsensusAndBody(mode)) {
    return {};
  }
  auto consensusCheck = checkBlockConsensus(block, consensus);
  if (!consensusCheck) {
    return consensusCheck;
  }
  return checkBlockBodyPolicy(block, bank, ledger, consensus, optChainConfig,
                              checkpoint, recordHandler);
}

} // namespace pp::chain_block
