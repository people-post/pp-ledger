#include "Chain.h"
#include "BlockValidation.h"
#include "ConfigTxHandler.h"
#include "DefaultTxHandler.h"
#include "EndUserTxHandler.h"
#include "GenesisRenewalTxHandler.h"
#include "GenesisTxHandler.h"
#include "NewUserTxHandler.h"
#include "TxFees.h"
#include "TxLedgerMeta.h"
#include "TxSignatures.h"
#include "UserUpdateTxHandler.h"
#include "RenewalUtil.h"
#include "../ledger/TypedTx.h"
#include "lib/common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
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

  auto installHandler = [this](std::size_t type,
                               std::unique_ptr<ITxHandler> handler,
                               const char *suffix) {
    handler->redirectLogger(log().getFullName() + "." + suffix);
    txHandlers_[type] = std::move(handler);
  };

  installHandler(Ledger::T_GENESIS,
                 std::make_unique<GenesisTxHandler>(), "GenesisTxHandler");
  installHandler(Ledger::T_CONFIG,
                 std::make_unique<ConfigTxHandler>(), "ConfigTxHandler");
  installHandler(Ledger::T_NEW_USER,
                 std::make_unique<NewUserTxHandler>(), "NewUserTxHandler");
  installHandler(Ledger::T_USER_UPDATE,
                 std::make_unique<UserUpdateTxHandler>(),
                 "UserUpdateTxHandler");
  installHandler(Ledger::T_DEFAULT,
                 std::make_unique<DefaultTxHandler>(), "DefaultTxHandler");
  installHandler(Ledger::T_RENEWAL,
                 std::make_unique<GenesisRenewalTxHandler>(),
                 "GenesisRenewalTxHandler");
  installHandler(Ledger::T_END_USER,
                 std::make_unique<EndUserTxHandler>(), "EndUserTxHandler");
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

bool Chain::shouldUseStrictMode(uint64_t blockIndex) const {
  if (txContext_.checkpoint.currentId == 0) {
    return true;
  }
  if (txContext_.checkpoint.currentId == txContext_.checkpoint.lastId) {
    // Not fully initialized yet
    return false;
  }
  return blockIndex >= txContext_.checkpoint.currentId;
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
  return userAccount;
}

Chain::Roe<Client::UserAccount>
Chain::getUserAccountMetaFromBlock(const Ledger::Block &block,
                                   uint64_t accountId) const {
  return mapTx(chain_tx::getUserAccountMetaFromBlock(block, accountId));
}

Chain::Roe<Chain::GenesisAccountMeta>
Chain::getGenesisAccountMetaFromBlock(const Ledger::Block &block) const {
  return mapTx(chain_tx::getGenesisAccountMetaFromBlock(block));
}

Chain::Roe<std::string> Chain::getUpdatedAccountMetadataForRenewal(
    const Ledger::Block &block, const AccountBuffer::Account &account,
    uint64_t minFee) const {
  return mapTx(
      chain_tx::getUpdatedAccountMetadataForRenewal(block, account, minFee));
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
    if (balance < minimumFee) {
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
Chain::collectRenewals(uint64_t slot) const {
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
      bool matches = false;
      switch (it->type) {
      case Ledger::T_DEFAULT: {
        auto txRoe = utl::binaryUnpack<Ledger::TxDefault>(it->data);
        if (txRoe) {
          const auto &tx = txRoe.value();
          matches = (tx.fromWalletId == walletId || tx.toWalletId == walletId);
        }
        break;
      }
      case Ledger::T_GENESIS: {
        auto txRoe = utl::binaryUnpack<Ledger::TxGenesis>(it->data);
        if (txRoe) {
          matches = (walletId == AccountBuffer::ID_GENESIS);
        }
        break;
      }
      case Ledger::T_NEW_USER: {
        auto txRoe = utl::binaryUnpack<Ledger::TxNewUser>(it->data);
        if (txRoe) {
          const auto &tx = txRoe.value();
          matches = (tx.fromWalletId == walletId || tx.toWalletId == walletId);
        }
        break;
      }
      case Ledger::T_CONFIG: {
        auto txRoe = utl::binaryUnpack<Ledger::TxConfig>(it->data);
        if (txRoe) {
          matches = (walletId == AccountBuffer::ID_GENESIS);
        }
        break;
      }
      case Ledger::T_USER_UPDATE: {
        auto txRoe = utl::binaryUnpack<Ledger::TxUserUpdate>(it->data);
        if (txRoe) {
          const auto &tx = txRoe.value();
          matches = (tx.walletId == walletId);
        }
        break;
      }
      case Ledger::T_RENEWAL: {
        auto txRoe = utl::binaryUnpack<Ledger::TxRenewal>(it->data);
        if (txRoe) {
          const auto &tx = txRoe.value();
          matches = (tx.walletId == walletId);
        }
        break;
      }
      case Ledger::T_END_USER: {
        auto txRoe = utl::binaryUnpack<Ledger::TxEndUser>(it->data);
        if (txRoe) {
          const auto &tx = txRoe.value();
          matches = (tx.walletId == walletId);
        }
        break;
      }
      default:
        break;
      }
      if (matches) {
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

void Chain::refreshStakeholders() {
  if (txContext_.consensus.isStakeUpdateNeeded()) {
    auto stakeholders = txContext_.bank.getStakeholders();
    txContext_.consensus.setStakeholders(stakeholders);
  }
}

void Chain::refreshStakeholders(uint64_t blockSlot) {
  uint64_t epoch = txContext_.consensus.getEpochFromSlot(blockSlot);
  if (txContext_.consensus.isStakeUpdateNeeded(epoch)) {
    auto stakeholders = txContext_.bank.getStakeholders();
    txContext_.consensus.setStakeholders(stakeholders, epoch);
  }
}

void Chain::initConsensus(const consensus::Ouroboros::Config &config) {
  txContext_.consensus.init(config);
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

  // Process blocks from ledger one by one (replay existing chain state)
  // Starting block id is always a checkpoint id
  txContext_.checkpoint.lastId = startingBlockId;
  txContext_.checkpoint.currentId = startingBlockId;
  uint64_t blockId = startingBlockId;
  uint64_t logInterval = 1000; // Log every 1000 blocks
  // Strict validatation if we are loading from the beginning
  bool isStrictMode = startingBlockId == 0;
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
    //   1. 0 block is using strict mode by default.
    //   2. Consensus parameters are initialized while processing the genesis
    //   transaction.
    if (blockId > 0) {
      refreshStakeholders(block.block.slot);
    }

    auto processResult = processBlock(block, isStrictMode);
    if (!processResult) {
      return Error(E_BLOCK_VALIDATION, "Failed to process block " +
                                           std::to_string(blockId) + ": " +
                                           processResult.error().message);
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
  bool isStrictMode = shouldUseStrictMode(block.block.index);
  auto processResult = processBlock(block, isStrictMode);
  if (!processResult) {
    return Error(E_BLOCK_VALIDATION,
                 "Failed to process block: " + processResult.error().message);
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

Chain::Roe<void> Chain::processBlock(const Ledger::ChainNode &block,
                                     bool isStrictMode) {
  if (block.block.index == 0) {
    return processGenesisBlock(block);
  } else {
    return processNormalBlock(block, isStrictMode);
  }
}

Chain::Roe<void> Chain::processGenesisBlock(const Ledger::ChainNode &block) {
  auto roe = mapTxVoid(chain_block::validateGenesisBlock(block));
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

  return {};
}

Chain::Roe<void> Chain::processNormalBlock(const Ledger::ChainNode &block,
                                           bool isStrictMode) {
  auto roe = mapTxVoid(chain_block::validateNormalBlock(
      block, isStrictMode, txContext_.ledger, txContext_.consensus,
      txContext_.bank, txContext_.optChainConfig, txContext_.checkpoint));
  if (!roe) {
    return Error(E_BLOCK_VALIDATION, "Block validation failed for block " +
                                         std::to_string(block.block.index) +
                                         ": " + roe.error().message);
  }

  for (const auto &rec : block.block.records) {
    auto result = processNormalTxRecord(rec, block.block.index, block.block.slot,
                                        block.block.slotLeader, isStrictMode);
    if (!result) {
      return Error(E_TX_VALIDATION,
                   "Failed to process transaction: " + result.error().message);
    }
  }

  if (txContext_.optChainConfig.has_value() &&
      needsCheckpoint(txContext_.optChainConfig.value()) &&
      block.block.index > txContext_.checkpoint.currentId) {
    txContext_.checkpoint.lastId = txContext_.checkpoint.currentId;
    txContext_.checkpoint.currentId = block.block.index;
    log().info << "Checkpoint rotated: last=" << txContext_.checkpoint.lastId
               << ", current=" << txContext_.checkpoint.currentId;
  }

  return {};
}

Chain::Roe<void> Chain::addBufferTransaction(
    AccountBuffer &bank,
    const Ledger::Record &record,
    uint64_t slotLeaderId) const {
  auto roe = validateTxSignatures(record, slotLeaderId, true);
  if (!roe) {
    return Error(E_TX_SIGNATURE, "Failed to validate buffer transaction: " +
                                     roe.error().message);
  }

  auto blockId = getNextBlockId();
  const uint64_t currentSlot = getCurrentSlot();
  auto typedRoe = decodeRecordToTypedTx(record);
  if (!typedRoe) {
    return Error(E_INVALID_ARGUMENT,
                 "Invalid packed transaction payload: " +
                     typedRoe.error().message);
  }

  auto &handler = txHandlers_[record.type];
  if (!handler) {
    return Error(E_INTERNAL, "Transaction handler not registered for type " +
                                std::to_string(record.type));
  }
  BufferApplyContext ctx{ txContext_,
                          blockId,
                          currentSlot,
                          true };
  return mapTxVoid(handler->applyBuffer(typedRoe.value(), bank, ctx));
}

Chain::Roe<void> Chain::processGenesisTxRecord(
    const Ledger::Record &record) {
  auto roe = validateTxSignatures(record, 0, true);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  auto typedRoe = decodeRecordToTypedTx(record);
  if (!typedRoe) {
    return Error(E_INVALID_ARGUMENT,
                 "Invalid packed transaction payload: " +
                     typedRoe.error().message);
  }

  switch (record.type) {
  case Ledger::T_GENESIS: {
    auto &h = txHandlers_[Ledger::T_GENESIS];
    if (!h) {
      return Error(E_INTERNAL, "Genesis transaction handler not registered");
    }
    const auto *tx = std::get_if<Ledger::TxGenesis>(&typedRoe.value());
    if (!tx) {
      return Error(E_TX_TYPE, "Unknown transaction type in genesis block: " +
                                  std::to_string(record.type));
    }
    return mapTxVoid(h->applyGenesisInit(*tx, txContext_));
  }
  case Ledger::T_NEW_USER:
  {
    const auto *tx = std::get_if<Ledger::TxNewUser>(&typedRoe.value());
    if (!tx) {
      return Error(E_TX_TYPE, "Unknown transaction type in genesis block: " +
                                  std::to_string(record.type));
    }
    return processUserInit(*tx, 0, true);
  }
  default:
    return Error(E_TX_TYPE, "Unknown transaction type in genesis block: " +
                                std::to_string(record.type));
  }
}

Chain::Roe<void> Chain::processNormalTxRecord(
    const Ledger::Record &record, uint64_t blockId,
    uint64_t blockSlot, uint64_t slotLeaderId, bool isStrictMode) {
  auto roe = validateTxSignatures(record, slotLeaderId, isStrictMode);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  auto typedRoe = decodeRecordToTypedTx(record);
  if (!typedRoe) {
    return Error(E_INVALID_ARGUMENT,
                 "Invalid packed transaction payload: " +
                     typedRoe.error().message);
  }

  auto &handler = txHandlers_[record.type];
  if (!handler) {
    return Error(E_INTERNAL, "Transaction handler not registered for type " +
                                 std::to_string(record.type));
  }
  BlockApplyContext ctx{ txContext_,
                         blockId,
                         blockSlot,
                         slotLeaderId,
                         isStrictMode };
  return mapTxVoid(handler->applyBlock(typedRoe.value(), txContext_.bank, ctx));
}

Chain::Roe<void> Chain::verifySignaturesAgainstAccount(
    const std::string &message, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account) const {
  return mapTxVoid(chain_tx::verifySignaturesAgainstAccount(
      message, signatures, account, txContext_.crypto, log()));
}

Chain::Roe<void> Chain::validateTxSignatures(
    const Ledger::Record &record,
    uint64_t slotLeaderId, bool isStrictMode) const {
  if (record.signatures.size() < 1) {
    return Error(E_TX_SIGNATURE,
                 "Transaction must have at least one signature");
  }

  auto typedRoe = decodeRecordToTypedTx(record);
  if (!typedRoe) {
    return Error(E_INVALID_ARGUMENT,
                 "Invalid packed transaction payload: " +
                     typedRoe.error().message);
  }

  auto &handler = txHandlers_[record.type];
  if (!handler) {
    return Error(E_INTERNAL, "Transaction handler not registered for type " +
                                 std::to_string(record.type));
  }

  auto signerAccountIdRoe =
      handler->getSignerAccountId(typedRoe.value(), slotLeaderId);
  if (!signerAccountIdRoe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to resolve signer account id: " +
                     signerAccountIdRoe.error().message);
  }
  const uint64_t signerAccountId = signerAccountIdRoe.value();

  auto accountResult = txContext_.bank.getAccount(signerAccountId);
  if (!accountResult) {
    if (isStrictMode) {
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
      // In loose mode, account may not be created before their transactions
      return {};
    }
  }
  return verifySignaturesAgainstAccount(record.data, record.signatures,
                                        accountResult.value());
}

Chain::Roe<void> Chain::processUserInit(const Ledger::TxNewUser &tx,
                                        uint64_t blockId, bool isStrictMode) {
  log().info << "Processing user initialization transaction";
  auto &h = txHandlers_[Ledger::T_NEW_USER];
  if (!h) {
    return Error(E_INTERNAL, "New user transaction handler not registered");
  }
  auto result = mapTxVoid(h->applyNewUser(tx, txContext_, txContext_.bank,
                                          blockId, false, isStrictMode));
  if (!result) {
    return result.error();
  }
  log().info << "Added new user " << tx.toWalletId;
  return {};
}

Chain::Roe<uint64_t>
Chain::calculateMinimumFeeForAccountMeta(const AccountBuffer &bank,
                                         uint64_t accountId) const {
  if (!txContext_.optChainConfig.has_value()) {
    return Error(E_INTERNAL,
                 "Chain config required for minimum fee from account meta");
  }
  return mapTx(chain_tx::calculateMinimumFeeForAccountMeta(
      txContext_.ledger, txContext_.optChainConfig.value(), bank, accountId));
}

} // namespace pp
