#include "BlockValidation.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "lib/common/Logger.h"
#include "lib/common/Utilities.h"

#include <limits>
#include <set>
#include <utility>

namespace pp::chain_block {

namespace {

bool isValidSlotLeader(const consensus::Ouroboros &consensus,
                       const Ledger::ChainNode &block) {
  return consensus.isSlotLeader(block.block.slot, block.block.slotLeader);
}

bool isValidTimestamp(const consensus::Ouroboros &consensus,
                      const Ledger::ChainNode &block) {
  int64_t slotStartTime = consensus.getSlotStartTime(block.block.slot);
  int64_t slotEndTime = consensus.getSlotEndTime(block.block.slot);
  int64_t blockTime = block.block.timestamp;
  if (blockTime < slotStartTime || blockTime > slotEndTime) {
    pp::logging::getLogger("Chain").warning << "Block timestamp out of slot range";
    return false;
  }
  return true;
}

} // namespace

std::string calculateBlockHash(const Ledger::Block &block) {
  std::string serialized = block.ltsToString();
  return utl::sha256(serialized);
}

chain_tx::Roe<void> validateGenesisBlock(const Ledger::ChainNode &block) {
  if (block.block.index != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have index 0");
  }
  if (block.block.previousHash != "0") {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have previousHash \"0\"");
  }
  if (block.block.nonce != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have nonce 0");
  }
  if (block.block.slot != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have slot 0");
  }
  if (block.block.slotLeader != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have slotLeader 0");
  }
  if (block.block.txIndex != 0) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis block must have txIndex 0");
  }
  if (block.block.records.size() != 4) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis block must have exactly four transactions");
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

  const auto &feeRec = block.block.records[1];
  if (feeRec.type != Ledger::T_NEW_USER) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Second genesis transaction must be new user transaction");
  }
  auto feeTxRoe = utl::binaryUnpack<Ledger::TxNewUser>(feeRec.data);
  if (!feeTxRoe) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Failed to deserialize fee tx payload");
  }
  const auto &feeTx = feeTxRoe.value();
  if (feeTx.fromWalletId != AccountBuffer::ID_GENESIS ||
      feeTx.toWalletId != AccountBuffer::ID_FEE) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis fee account creation transaction "
        "must transfer from genesis to fee wallet");
  }
  if (feeTx.amount != 0) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis fee account creation transaction must have amount 0");
  }
  const Ledger::TypedTx feeTypedTx(feeTx);
  auto feeWalletFeeResult =
      chain_tx::calculateMinimumFeeForTransaction(gm.config, feeTypedTx);
  if (!feeWalletFeeResult) {
    return chain_tx::Roe<void>(feeWalletFeeResult.error());
  }
  const uint64_t expectedFeeWalletFee = feeWalletFeeResult.value();
  if (feeTx.fee != expectedFeeWalletFee) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis fee account creation transaction must have fee: " +
            std::to_string(expectedFeeWalletFee));
  }
  if (feeTx.meta.empty()) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis fee account creation transaction must have meta");
  }

  const auto &minerRec = block.block.records[2];
  if (minerRec.type != Ledger::T_NEW_USER) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Third genesis transaction must be new user transaction");
  }
  auto minerTxRoe = utl::binaryUnpack<Ledger::TxNewUser>(minerRec.data);
  if (!minerTxRoe) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Failed to deserialize reserve tx payload");
  }
  const auto &minerTx = minerTxRoe.value();
  if (minerTx.fromWalletId != AccountBuffer::ID_GENESIS ||
      minerTx.toWalletId != AccountBuffer::ID_RESERVE) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis miner transaction must transfer "
                             "from genesis to new user wallet");
  }
  const Ledger::TypedTx minerTypedTx(minerTx);
  auto reserveFeeResult =
      chain_tx::calculateMinimumFeeForTransaction(gm.config, minerTypedTx);
  if (!reserveFeeResult) {
    return chain_tx::Roe<void>(reserveFeeResult.error());
  }
  const uint64_t expectedReserveFee = reserveFeeResult.value();
  if (minerTx.fee != expectedReserveFee) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis reserve transaction must have fee: " +
            std::to_string(expectedReserveFee));
  }

  const auto &recycleRec = block.block.records[3];
  if (recycleRec.type != Ledger::T_NEW_USER) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Fourth genesis transaction must be new user transaction");
  }
  auto recycleTxRoe = utl::binaryUnpack<Ledger::TxNewUser>(recycleRec.data);
  if (!recycleTxRoe) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Failed to deserialize recycle tx payload");
  }
  const auto &recycleTx = recycleTxRoe.value();
  const Ledger::TypedTx recycleTypedTx(recycleTx);
  auto recycleFeeResult =
      chain_tx::calculateMinimumFeeForTransaction(gm.config, recycleTypedTx);
  if (!recycleFeeResult) {
    return chain_tx::Roe<void>(recycleFeeResult.error());
  }
  const uint64_t expectedRecycleFee = recycleFeeResult.value();

  if (minerTx.fee >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      recycleTx.fee >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS,
                             "Genesis transaction fee exceeds int64_t range");
  }
  const int64_t minerFeeSigned = static_cast<int64_t>(minerTx.fee);
  const int64_t recycleFeeSigned = static_cast<int64_t>(recycleTx.fee);

  if (feeTx.fee >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis fee-wallet transaction fee exceeds int64_t range");
  }
  const int64_t feeWalletFeeSigned = static_cast<int64_t>(feeTx.fee);

  if (minerTx.amount + feeWalletFeeSigned + minerFeeSigned +
          recycleFeeSigned !=
      AccountBuffer::INITIAL_TOKEN_SUPPLY) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis reserve+recycle transactions must satisfy amount + "
        "fees: " +
            std::to_string(AccountBuffer::INITIAL_TOKEN_SUPPLY));
  }

  if (recycleTx.fromWalletId != AccountBuffer::ID_GENESIS ||
      recycleTx.toWalletId != AccountBuffer::ID_RECYCLE) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis recycle account creation transaction must transfer "
        "from genesis to recycle wallet");
  }
  if (recycleTx.amount != 0) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis recycle account creation transaction must have amount 0");
  }
  if (recycleTx.fee != expectedRecycleFee) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis recycle account creation transaction must have fee: " +
            std::to_string(expectedRecycleFee));
  }
  if (recycleTx.meta.empty()) {
    return chain_tx::TxError(
        chain_err::E_BLOCK_GENESIS,
        "Genesis recycle account creation transaction must have meta");
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
    auto typedRoe = pp::Ledger::decodeRecord(rec);
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

uint64_t getBlockAgeSeconds(uint64_t blockId, const Ledger &ledger,
                            const consensus::Ouroboros &consensus) {
  auto blockResult = ledger.readBlock(blockId);
  if (!blockResult) {
    return 0;
  }
  auto block = blockResult.value();
  auto currentTime = consensus.getTimestamp();
  int64_t blockTime = block.block.timestamp;
  if (currentTime > blockTime) {
    return static_cast<uint64_t>(currentTime - blockTime);
  }
  return 0;
}

bool needsCheckpoint(const BlockChainConfig &config,
                     const Checkpoint &checkpoint, uint64_t nextBlockId,
                     uint64_t checkpointBlockAgeSeconds) {
  uint64_t margin = config.slotsPerEpoch;
  const uint64_t requiredBlocks =
      checkpoint.currentId + config.checkpoint.minBlocks + margin;
  if (nextBlockId < requiredBlocks) {
    return false;
  }
  if (checkpointBlockAgeSeconds < config.checkpoint.minAgeSeconds) {
    return false;
  }
  return true;
}

chain_tx::Roe<uint64_t> calculateMaxBlockIdForRenewal(
    const Ledger &ledger, const consensus::Ouroboros &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, uint64_t atBlockId) {
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
    const int64_t cutoffTimestamp =
        consensus.getTimestamp() - static_cast<int64_t>(minAgeSeconds);
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
    const Ledger &ledger, const consensus::Ouroboros &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, const RecordHandler &recordHandler) {
  auto maxBlockIdResult = calculateMaxBlockIdForRenewal(
      ledger, consensus, optChainConfig, checkpoint, block.block.index);
  if (!maxBlockIdResult) {
    return chain_tx::Roe<void>(maxBlockIdResult.error());
  }
  const uint64_t maxBlockIdForRenewal = maxBlockIdResult.value();

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
    auto typedRoe = pp::Ledger::decodeRecord(rec);
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

    if (maxBlockIdForRenewal > 0 && account.blockId > maxBlockIdForRenewal) {
      return chain_tx::TxError(
          chain_err::E_ACCOUNT_RENEWAL,
          "Account renewal too early: account " + std::to_string(accountId) +
              " has blockId " + std::to_string(account.blockId) +
              " but deadline is at blockId " +
              std::to_string(maxBlockIdForRenewal));
    }

    accountsRenewedInBlock.insert(accountId);
  }

  for (uint64_t accountId : accountsNeedingRenewal) {
    if (accountsRenewedInBlock.find(accountId) ==
        accountsRenewedInBlock.end()) {
      return chain_tx::TxError(
          chain_err::E_ACCOUNT_RENEWAL,
          "Missing required account renewal: account " +
              std::to_string(accountId) +
              " meets renewal deadline but is not included in block");
    }
  }

  return {};
}

chain_tx::Roe<void>
validateNormalBlock(const Ledger::ChainNode &block, bool isStrictMode,
                     const Ledger &ledger, const consensus::Ouroboros &consensus,
                     const AccountBuffer &bank,
                     const std::optional<BlockChainConfig> &optChainConfig,
                     const Checkpoint &checkpoint,
                     const RecordHandler &recordHandler) {
  std::string calculatedHash = calculateBlockHash(block.block);
  if (calculatedHash != block.hash) {
    return chain_tx::TxError(chain_err::E_BLOCK_HASH,
                             "Block hash validation failed");
  }

  auto sequenceValidation = validateBlockSequence(ledger, block);
  if (!sequenceValidation) {
    return sequenceValidation;
  }

  if (isStrictMode) {
    uint64_t slot = block.block.slot;
    uint64_t slotLeader = block.block.slotLeader;
    if (!consensus.validateSlotLeader(slotLeader, slot)) {
      return chain_tx::TxError(
          chain_err::E_CONSENSUS_SLOT_LEADER,
          "Invalid slot leader for block at slot " + std::to_string(slot));
    }
    if (!consensus.validateBlockTiming(block.block.timestamp, slot)) {
      return chain_tx::TxError(chain_err::E_CONSENSUS_TIMING,
                               "Block timestamp outside valid slot range");
    }

    if (!isValidSlotLeader(consensus, block)) {
      return chain_tx::TxError(chain_err::E_CONSENSUS_SLOT_LEADER,
                               "Invalid slot leader");
    }

    if (!isValidTimestamp(consensus, block)) {
      return chain_tx::TxError(chain_err::E_CONSENSUS_TIMING,
                               "Invalid timestamp");
    }

    auto renewalValidation = validateAccountRenewals(
        block, bank, ledger, consensus, optChainConfig, checkpoint,
        recordHandler);
    if (!renewalValidation) {
      return renewalValidation;
    }

    if (!optChainConfig.has_value()) {
      return chain_tx::TxError(
          chain_err::E_INTERNAL,
          "Chain config not initialized; expected config in strict mode");
    }
    const uint64_t maxTx =
        optChainConfig.value().maxTransactionsPerBlock;
    if (maxTx > 0 && block.block.records.size() > maxTx) {
      for (const auto &rec : block.block.records) {
        const ITxHandler *h =
            rec.type < RecordHandler::kNumTxTypes
                ? recordHandler.get(rec.type)
                : nullptr;
        if (h == nullptr || !h->isRenewalTx()) {
          return chain_tx::TxError(
              chain_err::E_BLOCK_VALIDATION,
              "Block has more than max transactions per block (" +
                  std::to_string(block.block.records.size()) + " > " +
                  std::to_string(maxTx) +
                  ") but contains non-renewal transaction");
        }
      }
    }
    auto intraBlockIdem = validateIntraBlockIdempotency(block, recordHandler);
    if (!intraBlockIdem) {
      return intraBlockIdem;
    }
  }

  return {};
}

} // namespace pp::chain_block
