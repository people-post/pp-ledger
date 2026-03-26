#include "Chain.h"
#include "ConfigTxHandler.h"
#include "DefaultTxHandler.h"
#include "EndUserTxHandler.h"
#include "GenesisRenewalTxHandler.h"
#include "GenesisTxHandler.h"
#include "NewUserTxHandler.h"
#include "TxFees.h"
#include "TxIdempotency.h"
#include "TxLedgerMeta.h"
#include "TxSignatures.h"
#include "UserTxHandler.h"
#include "lib/common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <set>
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

  installHandler(Ledger::Transaction::T_GENESIS,
                 std::make_unique<GenesisTxHandler>(), "GenesisTxHandler");
  installHandler(Ledger::Transaction::T_CONFIG,
                 std::make_unique<ConfigTxHandler>(), "ConfigTxHandler");
  installHandler(Ledger::Transaction::T_NEW_USER,
                 std::make_unique<NewUserTxHandler>(), "NewUserTxHandler");
  installHandler(Ledger::Transaction::T_USER, std::make_unique<UserTxHandler>(),
                 "UserTxHandler");
  installHandler(Ledger::Transaction::T_DEFAULT,
                 std::make_unique<DefaultTxHandler>(), "DefaultTxHandler");
  installHandler(Ledger::Transaction::T_RENEWAL,
                 std::make_unique<GenesisRenewalTxHandler>(),
                 "GenesisRenewalTxHandler");
  installHandler(Ledger::Transaction::T_END_USER,
                 std::make_unique<EndUserTxHandler>(), "EndUserTxHandler");
}

bool Chain::isStakeholderSlotLeader(uint64_t stakeholderId,
                                    uint64_t slot) const {
  return txContext_.consensus.isSlotLeader(slot, stakeholderId);
}

bool Chain::isSlotBlockProductionTime(uint64_t slot) const {
  return txContext_.consensus.isSlotBlockProductionTime(slot);
}

bool Chain::isValidSlotLeader(const Ledger::ChainNode &block) const {
  return txContext_.consensus.isSlotLeader(block.block.slot,
                                           block.block.slotLeader);
}

bool Chain::isValidTimestamp(const Ledger::ChainNode &block) const {
  int64_t slotStartTime =
      txContext_.consensus.getSlotStartTime(block.block.slot);
  int64_t slotEndTime = txContext_.consensus.getSlotEndTime(block.block.slot);

  int64_t blockTime = block.block.timestamp;

  if (blockTime < slotStartTime || blockTime > slotEndTime) {
    log().warning << "Block timestamp out of slot range";
    return false;
  }

  return true;
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

Chain::Roe<void>
Chain::validateBlockSequence(const Ledger::ChainNode &block) const {
  const uint64_t startingBlockId = txContext_.ledger.getStartingBlockId();
  if (block.block.index < startingBlockId) {
    return Error(E_BLOCK_INDEX, "Invalid block index: expected >= " +
                                    std::to_string(startingBlockId) + " got " +
                                    std::to_string(block.block.index));
  }

  const uint64_t nextBlockId = txContext_.ledger.getNextBlockId();
  if (block.block.index > nextBlockId) {
    return Error(E_BLOCK_INDEX, "Invalid block index: expected <= " +
                                    std::to_string(nextBlockId) + " got " +
                                    std::to_string(block.block.index));
  }

  // For the first block in this ledger range, there is no previous block.
  if (block.block.index > startingBlockId) {
    auto prevBlockResult = txContext_.ledger.readBlock(block.block.index - 1);
    if (!prevBlockResult) {
      return Error(E_BLOCK_NOT_FOUND,
                   "Latest block not found: " +
                       std::to_string(block.block.index - 1));
    }
    auto prevBlock = prevBlockResult.value();

    if (block.block.index != prevBlock.block.index + 1) {
      return Error(E_BLOCK_INDEX,
                   "Invalid block index: expected " +
                       std::to_string(prevBlock.block.index + 1) + " got " +
                       std::to_string(block.block.index));
    }

    // Check previous hash matches
    if (block.block.previousHash != prevBlock.hash) {
      return Error(E_BLOCK_HASH, "Invalid previous hash: expected " +
                                     prevBlock.hash + " got " +
                                     block.block.previousHash);
    }

    // txIndex must equal previous block's cumulative transaction count
    const uint64_t expectedTxIndex =
        prevBlock.block.txIndex + prevBlock.block.signedTxes.size();
    if (block.block.txIndex != expectedTxIndex) {
      return Error(E_BLOCK_INDEX, "Invalid txIndex: expected " +
                                      std::to_string(expectedTxIndex) +
                                      " got " +
                                      std::to_string(block.block.txIndex));
    }
  }

  return {};
}

bool Chain::needsCheckpoint(const BlockChainConfig &config) const {
  // Keep checkpoints at least one epoch beyond the renewal span to avoid
  // edge cases where renewal windows and checkpoint boundaries overlap in
  // strict mode. This is a conservative safety margin.
  uint64_t margin = config.slotsPerEpoch;

  const uint64_t requiredBlocks =
      txContext_.checkpoint.currentId + config.checkpoint.minBlocks + margin;

  if (getNextBlockId() < requiredBlocks) {
    return false;
  }
  if (getBlockAgeSeconds(txContext_.checkpoint.currentId) <
      config.checkpoint.minAgeSeconds) {
    return false;
  }
  return true;
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

uint64_t Chain::getBlockAgeSeconds(uint64_t blockId) const {
  auto blockResult = txContext_.ledger.readBlock(blockId);
  if (!blockResult) {
    return 0;
  }
  auto block = blockResult.value();

  auto currentTime = txContext_.consensus.getTimestamp();
  int64_t blockTime = block.block.timestamp;

  if (currentTime > blockTime) {
    return static_cast<uint64_t>(currentTime - blockTime);
  }

  return 0;
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

Chain::Roe<Ledger::SignedData<Ledger::Transaction>>
Chain::createRenewalTx(uint64_t accountId) const {
  auto accountResult = txContext_.bank.getAccount(accountId);
  if (!accountResult) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "Account not found: " + std::to_string(accountId));
  }

  auto const &account = accountResult.value();
  Ledger::Transaction tx;
  tx.type = Ledger::Transaction::T_RENEWAL;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = accountId;
  tx.toWalletId = accountId;
  tx.amount = 0;
  tx.idempotentId = 0;
  tx.validationTsMin = 0;
  tx.validationTsMax = 0;

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
      tx.type = Ledger::Transaction::T_END_USER;
      tx.fee = 0;
    }
  }

  if (tx.type == Ledger::Transaction::T_RENEWAL) {
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

  Ledger::SignedData<Ledger::Transaction> signedTx;
  signedTx.obj = tx;
  return signedTx;
}

Chain::Roe<void>
Chain::validateAccountRenewals(const Ledger::ChainNode &block) const {
  // Calculate the deadline for account renewals at this block
  auto maxBlockIdResult = calculateMaxBlockIdForRenewal(block.block.index);
  if (!maxBlockIdResult) {
    return maxBlockIdResult.error();
  }
  const uint64_t maxBlockIdForRenewal = maxBlockIdResult.value();

  // Get accounts that must be renewed (blockId < maxBlockIdForRenewal)
  std::set<uint64_t> accountsNeedingRenewal;
  if (maxBlockIdForRenewal > 0) {
    for (uint64_t accountId :
         txContext_.bank.getAccountIdsBeforeBlockId(maxBlockIdForRenewal)) {
      accountsNeedingRenewal.insert(accountId);
    }
  }

  // Track which accounts are actually renewed in the block
  std::set<uint64_t> accountsRenewedInBlock;

  // Examine all transactions in the block
  for (const auto &signedTx : block.block.signedTxes) {
    const auto &tx = signedTx.obj;

    // Check for renewal and end-user transactions
    if (tx.type == Ledger::Transaction::T_RENEWAL ||
        tx.type == Ledger::Transaction::T_END_USER) {
      uint64_t accountId = tx.fromWalletId;

      // Get the account's current blockId
      auto accountResult = txContext_.bank.getAccount(accountId);
      if (!accountResult) {
        return Error(E_ACCOUNT_RENEWAL,
                     "Account not found in renewal transaction: " +
                         std::to_string(accountId));
      }
      const auto &account = accountResult.value();

      // Verify renewal is not too early (at most 1 block ahead of deadline)
      // An account with blockId >= maxBlockIdForRenewal is being renewed too
      // early We allow renewals for accounts with blockId <
      // maxBlockIdForRenewal (must renew) and blockId == maxBlockIdForRenewal
      // (1 block ahead, acceptable)
      if (maxBlockIdForRenewal > 0 && account.blockId > maxBlockIdForRenewal) {
        return Error(E_ACCOUNT_RENEWAL,
                     "Account renewal too early: account " +
                         std::to_string(accountId) + " has blockId " +
                         std::to_string(account.blockId) +
                         " but deadline is at blockId " +
                         std::to_string(maxBlockIdForRenewal));
      }

      accountsRenewedInBlock.insert(accountId);
    }
  }

  // Verify all accounts that need renewal are included
  for (uint64_t accountId : accountsNeedingRenewal) {
    if (accountsRenewedInBlock.find(accountId) ==
        accountsRenewedInBlock.end()) {
      return Error(E_ACCOUNT_RENEWAL,
                   "Missing required account renewal: account " +
                       std::to_string(accountId) +
                       " meets renewal deadline but is not included in block");
    }
  }

  return {};
}

Chain::Roe<uint64_t>
Chain::calculateMaxBlockIdForRenewal(uint64_t atBlockId) const {
  if (!txContext_.optChainConfig.has_value()) {
    if (txContext_.checkpoint.currentId > txContext_.checkpoint.lastId) {
      return Error(E_INTERNAL,
                   "Chain config not initialized; expected config when "
                   "checkpoint.currentId > checkpoint.lastId");
    }
    return 0; // No renewals while still syncing
  }
  const BlockChainConfig &config = txContext_.optChainConfig.value();
  const uint64_t minBlocks = config.checkpoint.minBlocks;
  if (atBlockId < minBlocks) {
    return 0;
  }
  uint64_t maxBlockIdFromBlocks = atBlockId - minBlocks + 1;

  const uint64_t minAgeSeconds = config.checkpoint.minAgeSeconds;

  uint64_t maxBlockIdFromTime = atBlockId;
  if (minAgeSeconds > 0 && atBlockId > 0) {
    const int64_t cutoffTimestamp =
        getConsensusTimestamp() - static_cast<int64_t>(minAgeSeconds);
    auto roeBlock = txContext_.ledger.findBlockByTimestamp(cutoffTimestamp);
    if (roeBlock) {
      maxBlockIdFromTime = roeBlock.value().block.index;
    }
  }
  const uint64_t maxBlockIdForRenewal =
      std::min(maxBlockIdFromBlocks, maxBlockIdFromTime);
  if (maxBlockIdForRenewal == 0 || maxBlockIdForRenewal >= atBlockId) {
    // maxBlockIdForRenewal is capped at current block id
    return 0;
  }

  return maxBlockIdForRenewal;
}

Chain::Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>>
Chain::collectRenewals(uint64_t slot) const {
  std::vector<Ledger::SignedData<Ledger::Transaction>> renewals;
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

Chain::Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>>
Chain::findTransactionsByWalletId(uint64_t walletId,
                                  uint64_t &ioBlockId) const {
  // ioBlockId is the block ID to start scanning from (exclusive). It is updated
  // to the last scanned block ID. Client sends 0 to mean "latest" (scan from
  // tip); we substitute getNextBlockId() so that scanning runs.

  std::vector<Ledger::SignedData<Ledger::Transaction>> out;
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
    auto const &txes = blockRoe.value().block.signedTxes;
    for (auto it = txes.rbegin(); it != txes.rend(); ++it) {
      const Ledger::Transaction &tx = it->obj;
      if (tx.fromWalletId == walletId || tx.toWalletId == walletId) {
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

Chain::Roe<Ledger::SignedData<Ledger::Transaction>>
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
      static_cast<uint64_t>(lastBlock.signedTxes.size());
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
        static_cast<uint64_t>(block.signedTxes.size());

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
    return block.signedTxes[static_cast<size_t>(localIndex)];
  }

  return Error(E_LEDGER_READ, "Transaction index " + std::to_string(txIndex) +
                                  " not found in any block");
}

std::string Chain::calculateHash(const Ledger::Block &block) const {
  // Use ltsToString() to get the serialized block representation
  std::string serialized = block.ltsToString();
  return utl::sha256(serialized);
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

Chain::Roe<void>
Chain::validateGenesisBlock(const Ledger::ChainNode &block) const {
  // Match Beacon::createGenesisBlock exactly: index 0, previousHash "0", nonce
  // 0, slot 0, slotLeader 0
  if (block.block.index != 0) {
    return Error(E_BLOCK_GENESIS, "Genesis block must have index 0");
  }
  if (block.block.previousHash != "0") {
    return Error(E_BLOCK_GENESIS, "Genesis block must have previousHash \"0\"");
  }
  if (block.block.nonce != 0) {
    return Error(E_BLOCK_GENESIS, "Genesis block must have nonce 0");
  }
  if (block.block.slot != 0) {
    return Error(E_BLOCK_GENESIS, "Genesis block must have slot 0");
  }
  if (block.block.slotLeader != 0) {
    return Error(E_BLOCK_GENESIS, "Genesis block must have slotLeader 0");
  }
  if (block.block.txIndex != 0) {
    return Error(E_BLOCK_GENESIS, "Genesis block must have txIndex 0");
  }
  // Exactly four transactions: checkpoint, fee, reserve, and recycle
  if (block.block.signedTxes.size() != 4) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis block must have exactly four transactions");
  }

  // First transaction: checkpoint transaction (ID_GENESIS -> ID_GENESIS, amount
  // 0)
  const auto &checkpointTx = block.block.signedTxes[0];
  if (checkpointTx.obj.type != Ledger::Transaction::T_GENESIS) {
    return Error(E_BLOCK_GENESIS,
                 "First genesis transaction must be genesis transaction");
  }
  GenesisAccountMeta gm;
  if (!gm.ltsFromString(checkpointTx.obj.meta)) {
    return Error(E_BLOCK_GENESIS,
                 "Failed to deserialize genesis checkpoint meta");
  }

  // Second transaction: fee transaction (ID_GENESIS -> ID_FEE, 0)
  const auto &feeTx = block.block.signedTxes[1];
  if (feeTx.obj.type != Ledger::Transaction::T_NEW_USER) {
    return Error(E_BLOCK_GENESIS,
                 "Second genesis transaction must be new user transaction");
  }
  if (feeTx.obj.fromWalletId != AccountBuffer::ID_GENESIS ||
      feeTx.obj.toWalletId != AccountBuffer::ID_FEE) {
    return Error(E_BLOCK_GENESIS, "Genesis fee account creation transaction "
                                  "must transfer from genesis to fee wallet");
  }
  if (feeTx.obj.amount != 0) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis fee account creation transaction must have amount 0");
  }
  auto feeWalletFeeResult = mapTx(
      chain_tx::calculateMinimumFeeForTransaction(gm.config, feeTx.obj));
  if (!feeWalletFeeResult) {
    return feeWalletFeeResult.error();
  }
  const uint64_t expectedFeeWalletFee = feeWalletFeeResult.value();
  if (feeTx.obj.fee != expectedFeeWalletFee) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis fee account creation transaction must have fee: " +
                     std::to_string(expectedFeeWalletFee));
  }
  if (feeTx.obj.meta.empty()) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis fee account creation transaction must have meta");
  }

  // Third transaction: miner/reserve transaction (ID_GENESIS -> ID_RESERVE,
  // INITIAL_TOKEN_SUPPLY)
  const auto &minerTx = block.block.signedTxes[2];
  if (minerTx.obj.type != Ledger::Transaction::T_NEW_USER) {
    return Error(E_BLOCK_GENESIS,
                 "Third genesis transaction must be new user transaction");
  }
  if (minerTx.obj.fromWalletId != AccountBuffer::ID_GENESIS ||
      minerTx.obj.toWalletId != AccountBuffer::ID_RESERVE) {
    return Error(E_BLOCK_GENESIS, "Genesis miner transaction must transfer "
                                  "from genesis to new user wallet");
  }
  auto reserveFeeResult = mapTx(
      chain_tx::calculateMinimumFeeForTransaction(gm.config, minerTx.obj));
  if (!reserveFeeResult) {
    return reserveFeeResult.error();
  }
  const uint64_t expectedReserveFee = reserveFeeResult.value();
  if (minerTx.obj.fee != expectedReserveFee) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis reserve transaction must have fee: " +
                     std::to_string(expectedReserveFee));
  }

  // Fourth transaction: recycle account creation (ID_GENESIS -> ID_RECYCLE, 0)
  const auto &recycleTx = block.block.signedTxes[3];
  auto recycleFeeResult = mapTx(
      chain_tx::calculateMinimumFeeForTransaction(gm.config, recycleTx.obj));
  if (!recycleFeeResult) {
    return recycleFeeResult.error();
  }
  const uint64_t expectedRecycleFee = recycleFeeResult.value();

  if (minerTx.obj.fee >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      recycleTx.obj.fee >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis transaction fee exceeds int64_t range");
  }
  const int64_t minerFeeSigned = static_cast<int64_t>(minerTx.obj.fee);
  const int64_t recycleFeeSigned = static_cast<int64_t>(recycleTx.obj.fee);

  if (feeTx.obj.fee >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis fee-wallet transaction fee exceeds int64_t range");
  }
  const int64_t feeWalletFeeSigned = static_cast<int64_t>(feeTx.obj.fee);

  if (minerTx.obj.amount + feeWalletFeeSigned + minerFeeSigned +
          recycleFeeSigned !=
      AccountBuffer::INITIAL_TOKEN_SUPPLY) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis reserve+recycle transactions must satisfy amount + "
                 "fees: " +
                     std::to_string(AccountBuffer::INITIAL_TOKEN_SUPPLY));
  }

  if (recycleTx.obj.type != Ledger::Transaction::T_NEW_USER) {
    return Error(E_BLOCK_GENESIS,
                 "Fourth genesis transaction must be new user transaction");
  }
  if (recycleTx.obj.fromWalletId != AccountBuffer::ID_GENESIS ||
      recycleTx.obj.toWalletId != AccountBuffer::ID_RECYCLE) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis recycle account creation transaction must transfer "
                 "from genesis to recycle wallet");
  }
  if (recycleTx.obj.amount != 0) {
    return Error(
        E_BLOCK_GENESIS,
        "Genesis recycle account creation transaction must have amount 0");
  }
  if (recycleTx.obj.fee != expectedRecycleFee) {
    return Error(
        E_BLOCK_GENESIS,
        "Genesis recycle account creation transaction must have fee: " +
            std::to_string(expectedRecycleFee));
  }
  if (recycleTx.obj.meta.empty()) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis recycle account creation transaction must have meta");
  }

  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
    return Error(E_BLOCK_HASH, "Genesis block hash validation failed");
  }
  return {};
}

Chain::Roe<void> Chain::validateNormalBlock(const Ledger::ChainNode &block,
                                            bool isStrictMode) const {
  // Non-genesis: validate slot leader and timing

  // Validate block hash
  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
    return Error(E_BLOCK_HASH, "Block hash validation failed");
  }

  // Validate sequence (hash chain, index, txIndex)
  auto sequenceValidation = validateBlockSequence(block);
  if (!sequenceValidation) {
    return sequenceValidation.error();
  }

  if (isStrictMode) {
    // Strict mode only checks
    uint64_t slot = block.block.slot;
    uint64_t slotLeader = block.block.slotLeader;
    if (!txContext_.consensus.validateSlotLeader(slotLeader, slot)) {
      return Error(E_CONSENSUS_SLOT_LEADER,
                   "Invalid slot leader for block at slot " +
                       std::to_string(slot));
    }
    if (!txContext_.consensus.validateBlockTiming(block.block.timestamp,
                                                  slot)) {
      return Error(E_CONSENSUS_TIMING,
                   "Block timestamp outside valid slot range");
    }

    // Validate slot leader
    if (!isValidSlotLeader(block)) {
      return Error(E_CONSENSUS_SLOT_LEADER, "Invalid slot leader");
    }

    // Validate timestamp
    if (!isValidTimestamp(block)) {
      return Error(E_CONSENSUS_TIMING, "Invalid timestamp");
    }

    // Validate account renewals in the block
    auto renewalValidation = validateAccountRenewals(block);
    if (!renewalValidation) {
      return renewalValidation.error();
    }

    // Validate max transactions per block: if total > max, all must be renewals
    if (!txContext_.optChainConfig.has_value()) {
      return Error(
          E_INTERNAL,
          "Chain config not initialized; expected config in strict mode");
    }
    const uint64_t maxTx =
        txContext_.optChainConfig.value().maxTransactionsPerBlock;
    if (maxTx > 0 && block.block.signedTxes.size() > maxTx) {
      for (const auto &signedTx : block.block.signedTxes) {
        if (signedTx.obj.type != Ledger::Transaction::T_RENEWAL) {
          return Error(E_BLOCK_VALIDATION,
                       "Block has more than max transactions per block (" +
                           std::to_string(block.block.signedTxes.size()) +
                           " > " + std::to_string(maxTx) +
                           ") but contains non-renewal transaction");
        }
      }
    }
    auto intraBlockIdem = validateIntraBlockIdempotency(block);
    if (!intraBlockIdem) {
      return intraBlockIdem.error();
    }
  }

  return {};
}

Chain::Roe<void>
Chain::validateIntraBlockIdempotency(const Ledger::ChainNode &block) const {
  // Validate that within a single block there is at most one transaction
  // per (fromWalletId, idempotentId) pair. Idempotency is per wallet and
  // idempotentId == 0 is treated as "no idempotency".
  std::set<std::pair<uint64_t, uint64_t>> seenIdempotentPairs;
  for (const auto &signedTx : block.block.signedTxes) {
    const auto &tx = signedTx.obj;
    if (tx.idempotentId == 0) {
      continue;
    }
    // Only enforce for transaction types that participate in idempotency rules.
    switch (tx.type) {
    case Ledger::Transaction::T_DEFAULT:
    case Ledger::Transaction::T_NEW_USER:
    case Ledger::Transaction::T_CONFIG:
    case Ledger::Transaction::T_USER: {
      auto key = std::make_pair(tx.fromWalletId, tx.idempotentId);
      if (!seenIdempotentPairs.insert(key).second) {
        return Error(E_TX_IDEMPOTENCY,
                     "Duplicate idempotent id within block: " +
                         std::to_string(tx.idempotentId) +
                         " for wallet: " + std::to_string(tx.fromWalletId));
      }
      break;
    }
    default:
      break;
    }
  }
  return {};
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
  // Validate the block first
  auto roe = validateGenesisBlock(block);
  if (!roe) {
    return Error(E_BLOCK_VALIDATION, "Block validation failed for block " +
                                         std::to_string(block.block.index) +
                                         ": " + roe.error().message);
  }

  // Process checkpoint transactions to restore BlockChainConfig
  for (const auto &signedTx : block.block.signedTxes) {
    auto result = processGenesisTxRecord(signedTx);
    if (!result) {
      return Error(E_TX_VALIDATION,
                   "Failed to process transaction: " + result.error().message);
    }
  }

  return {};
}

Chain::Roe<void> Chain::processNormalBlock(const Ledger::ChainNode &block,
                                           bool isStrictMode) {
  // Validate the block first
  auto roe = validateNormalBlock(block, isStrictMode);
  if (!roe) {
    return Error(E_BLOCK_VALIDATION, "Block validation failed for block " +
                                         std::to_string(block.block.index) +
                                         ": " + roe.error().message);
  }

  // Process checkpoint transactions to restore BlockChainConfig
  for (const auto &signedTx : block.block.signedTxes) {
    auto result =
        processNormalTxRecord(signedTx, block.block.index, block.block.slot,
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
    const Ledger::SignedData<Ledger::Transaction> &signedTx,
    uint64_t slotLeaderId) const {
  auto roe = validateTxSignatures(signedTx, slotLeaderId, true);
  if (!roe) {
    return Error(E_TX_SIGNATURE, "Failed to validate buffer transaction: " +
                                     roe.error().message);
  }

  auto blockId = getNextBlockId();
  const auto &tx = signedTx.obj;
  const uint64_t currentSlot = getCurrentSlot();
  switch (tx.type) {
  case Ledger::Transaction::T_DEFAULT: {
    auto idemRoe = validateIdempotencyRules(tx, currentSlot, true);
    if (!idemRoe) {
      return idemRoe.error();
    }
    return processBufferTx(bank, tx);
  }
  case Ledger::Transaction::T_NEW_USER: {
    auto idemRoe = validateIdempotencyRules(tx, currentSlot, true);
    if (!idemRoe) {
      return idemRoe.error();
    }
    if (tx.fee > 0) {
      auto feeRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_FEE);
      if (!feeRoe) {
        return feeRoe;
      }
    }
    return processBufferUserInit(bank, tx, blockId);
  }
  case Ledger::Transaction::T_CONFIG: {
    auto idemRoe = validateIdempotencyRules(tx, currentSlot, true);
    if (!idemRoe) {
      return idemRoe.error();
    }
    return processBufferSystemUpdate(bank, tx, blockId);
  }
  case Ledger::Transaction::T_USER: {
    auto idemRoe = validateIdempotencyRules(tx, currentSlot, true);
    if (!idemRoe) {
      return idemRoe.error();
    }
    return processBufferUserAccountUpsert(bank, tx, blockId);
  }
  case Ledger::Transaction::T_RENEWAL:
    if (tx.fromWalletId == AccountBuffer::ID_GENESIS) {
      return processBufferGenesisRenewal(bank, tx, blockId);
    }
    return processBufferUserAccountUpsert(bank, tx, blockId);
  case Ledger::Transaction::T_END_USER:
    return processBufferUserEnd(bank, tx);
  default:
    return Error(E_TX_TYPE,
                 "Unknown transaction type: " + std::to_string(tx.type));
  }
}

Chain::Roe<void> Chain::processGenesisTxRecord(
    const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  auto roe = validateTxSignatures(signedTx, 0, true);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  auto const &tx = signedTx.obj;
  switch (tx.type) {
  case Ledger::Transaction::T_GENESIS: {
    auto &h = txHandlers_[Ledger::Transaction::T_GENESIS];
    if (!h) {
      return Error(E_INTERNAL, "Genesis transaction handler not registered");
    }
    return mapTxVoid(h->applyGenesisInit(tx, txContext_));
  }
  case Ledger::Transaction::T_NEW_USER:
    return processUserInit(tx, 0, true);
  default:
    return Error(E_TX_TYPE, "Unknown transaction type in genesis block: " +
                                std::to_string(tx.type));
  }
}

Chain::Roe<void> Chain::processNormalTxRecord(
    const Ledger::SignedData<Ledger::Transaction> &signedTx, uint64_t blockId,
    uint64_t blockSlot, uint64_t slotLeaderId, bool isStrictMode) {
  auto roe = validateTxSignatures(signedTx, slotLeaderId, isStrictMode);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  auto const &tx = signedTx.obj;
  switch (tx.type) {
  case Ledger::Transaction::T_NEW_USER: {
    auto idemRoe = validateIdempotencyRules(tx, blockSlot, isStrictMode);
    if (!idemRoe) {
      return idemRoe.error();
    }
    return processUserInit(tx, blockId, isStrictMode);
  }
  case Ledger::Transaction::T_CONFIG: {
    auto idemRoe = validateIdempotencyRules(tx, blockSlot, isStrictMode);
    if (!idemRoe) {
      return idemRoe.error();
    }
    return processSystemUpdate(tx, blockId, isStrictMode);
  }
  case Ledger::Transaction::T_USER: {
    auto idemRoe = validateIdempotencyRules(tx, blockSlot, isStrictMode);
    if (!idemRoe) {
      return idemRoe.error();
    }
    return processUserUpdate(tx, blockId, isStrictMode);
  }
  case Ledger::Transaction::T_RENEWAL:
    if (tx.fromWalletId == AccountBuffer::ID_GENESIS) {
      return processGenesisRenewal(tx, blockId, isStrictMode);
    }
    return processUserRenewal(tx, blockId, isStrictMode);
  case Ledger::Transaction::T_END_USER:
    return processUserEnd(tx, blockId, isStrictMode);
  case Ledger::Transaction::T_DEFAULT: {
    auto idemRoe = validateIdempotencyRules(tx, blockSlot, isStrictMode);
    if (!idemRoe) {
      return idemRoe.error();
    }
    return processTx(tx, blockId, isStrictMode);
  }
  default:
    return Error(E_TX_TYPE,
                 "Unknown transaction type: " + std::to_string(tx.type));
  }
}

Chain::Roe<void> Chain::verifySignaturesAgainstAccount(
    const Ledger::Transaction &tx, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account) const {
  return mapTxVoid(chain_tx::verifySignaturesAgainstAccount(
      tx, signatures, account, txContext_.crypto, log()));
}

Chain::Roe<void> Chain::validateTxSignatures(
    const Ledger::SignedData<Ledger::Transaction> &signedTx,
    uint64_t slotLeaderId, bool isStrictMode) const {
  if (signedTx.signatures.size() < 1) {
    return Error(E_TX_SIGNATURE,
                 "Transaction must have at least one signature");
  }

  const auto &tx = signedTx.obj;
  uint64_t signerAccountId = tx.fromWalletId;

  // T_RENEWAL and T_END_USER are signed by the slot leader (miner), not by
  // fromWalletId
  if ((tx.type == Ledger::Transaction::T_RENEWAL ||
       tx.type == Ledger::Transaction::T_END_USER) &&
      slotLeaderId != 0) {
    signerAccountId = slotLeaderId;
  }

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
  return verifySignaturesAgainstAccount(tx, signedTx.signatures,
                                        accountResult.value());
}

Chain::Roe<void> Chain::checkIdempotency(uint64_t idempotentId,
                                         uint64_t fromWalletId,
                                         uint64_t slotMin,
                                         uint64_t slotMax) const {
  return mapTxVoid(
      chain_tx::checkIdempotency(txContext_.ledger, txContext_.consensus,
                                 idempotentId, fromWalletId, slotMin, slotMax));
}

Chain::Roe<void> Chain::validateIdempotencyRules(const Ledger::Transaction &tx,
                                                 uint64_t effectiveSlot,
                                                 bool isStrictMode) const {
  return mapTxVoid(chain_tx::validateIdempotencyRules(
      txContext_.ledger, txContext_.consensus, txContext_.optChainConfig, tx,
      effectiveSlot, isStrictMode));
}

Chain::Roe<void> Chain::processSystemUpdate(const Ledger::Transaction &tx,
                                            uint64_t blockId,
                                            bool isStrictMode) {
  auto &h = txHandlers_[Ledger::Transaction::T_CONFIG];
  if (!h) {
    return Error(E_INTERNAL, "Config transaction handler not registered");
  }
  return mapTxVoid(h->applyConfigUpdate(tx, txContext_, txContext_.bank,
                                        blockId, isStrictMode, true));
}

Chain::Roe<void> Chain::processGenesisRenewal(const Ledger::Transaction &tx,
                                              uint64_t blockId,
                                              bool isStrictMode) {
  log().info << "Processing genesis renewal transaction";
  auto &h = txHandlers_[Ledger::Transaction::T_RENEWAL];
  if (!h) {
    return Error(E_INTERNAL,
                 "Genesis renewal transaction handler not registered");
  }
  auto result = mapTxVoid(h->applyGenesisRenewal(
      tx, txContext_, txContext_.bank, blockId, false, isStrictMode));
  if (!result) {
    return result.error();
  }
  log().info << "Genesis account renewed";
  return {};
}

Chain::Roe<void> Chain::processUserInit(const Ledger::Transaction &tx,
                                        uint64_t blockId, bool isStrictMode) {
  log().info << "Processing user initialization transaction";
  auto &h = txHandlers_[Ledger::Transaction::T_NEW_USER];
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

Chain::Roe<void> Chain::processUserUpdate(const Ledger::Transaction &tx,
                                          uint64_t blockId, bool isStrictMode) {
  return processUserAccountUpsert(tx, blockId, isStrictMode);
}

Chain::Roe<void> Chain::processUserRenewal(const Ledger::Transaction &tx,
                                           uint64_t blockId,
                                           bool isStrictMode) {
  return processUserAccountUpsert(tx, blockId, isStrictMode);
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

Chain::Roe<void> Chain::processUserAccountUpsert(const Ledger::Transaction &tx,
                                                 uint64_t blockId,
                                                 bool isStrictMode) {
  log().info << "Processing user update/renewal transaction";
  auto &h = txHandlers_[Ledger::Transaction::T_USER];
  if (!h) {
    return Error(E_INTERNAL, "User transaction handler not registered");
  }
  auto result = mapTxVoid(h->applyUserAccountUpsert(
      tx, txContext_, txContext_.bank, blockId, false, isStrictMode));
  if (!result) {
    return result.error();
  }
  log().info << "User account " << tx.fromWalletId << " updated";
  return {};
}

Chain::Roe<void> Chain::processUserEnd(const Ledger::Transaction &tx,
                                       uint64_t blockId, bool isStrictMode) {
  (void)blockId;
  (void)isStrictMode;
  log().info << "Processing user end transaction";
  auto &h = txHandlers_[Ledger::Transaction::T_END_USER];
  if (!h) {
    return Error(E_INTERNAL, "End-user transaction handler not registered");
  }
  auto result =
      mapTxVoid(h->applyEndUser(tx, txContext_, txContext_.bank, false));
  if (!result) {
    return result.error();
  }
  log().info << "User account " << tx.fromWalletId << " ended";
  return {};
}

Chain::Roe<void> Chain::ensureAccountInBuffer(AccountBuffer &bank,
                                              uint64_t accountId) const {
  if (bank.hasAccount(accountId)) {
    return {};
  }
  if (!txContext_.bank.hasAccount(accountId)) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "Account not found: " + std::to_string(accountId));
  }
  auto accountResult = txContext_.bank.getAccount(accountId);
  if (!accountResult) {
    return Error(E_ACCOUNT_NOT_FOUND, "Failed to get account from bank: " +
                                          accountResult.error().message);
  }
  auto addResult = bank.add(accountResult.value());
  if (!addResult) {
    return Error(E_ACCOUNT_BUFFER, "Failed to add account to buffer: " +
                                       addResult.error().message);
  }
  return {};
}

Chain::Roe<void> Chain::processBufferUserInit(AccountBuffer &bank,
                                              const Ledger::Transaction &tx,
                                              uint64_t blockId) const {
  auto fromRoe = ensureAccountInBuffer(bank, tx.fromWalletId);
  if (!fromRoe) {
    return fromRoe;
  }
  auto &h = txHandlers_[Ledger::Transaction::T_NEW_USER];
  if (!h) {
    return Error(E_INTERNAL, "New user transaction handler not registered");
  }
  return mapTxVoid(h->applyNewUser(tx, txContext_, bank, blockId, true, true));
}

Chain::Roe<void> Chain::processBufferSystemUpdate(AccountBuffer &bank,
                                                  const Ledger::Transaction &tx,
                                                  uint64_t blockId) const {
  auto genesisRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_GENESIS);
  if (!genesisRoe) {
    return genesisRoe;
  }
  auto &h = txHandlers_[Ledger::Transaction::T_CONFIG];
  if (!h) {
    return Error(E_INTERNAL, "Config transaction handler not registered");
  }
  return mapTxVoid(h->applyConfigUpdate(tx, txContext_, bank, blockId, true));
}

Chain::Roe<void>
Chain::processBufferUserAccountUpsert(AccountBuffer &bank,
                                      const Ledger::Transaction &tx,
                                      uint64_t blockId) const {
  auto fromRoe = ensureAccountInBuffer(bank, tx.fromWalletId);
  if (!fromRoe) {
    return fromRoe;
  }
  if (tx.fee > 0 && tx.fromWalletId != AccountBuffer::ID_FEE) {
    auto feeRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_FEE);
    if (!feeRoe) {
      return feeRoe;
    }
  }
  auto &h = txHandlers_[Ledger::Transaction::T_USER];
  if (!h) {
    return Error(E_INTERNAL, "User transaction handler not registered");
  }
  return mapTxVoid(
      h->applyUserAccountUpsert(tx, txContext_, bank, blockId, true, true));
}

Chain::Roe<void>
Chain::processBufferGenesisRenewal(AccountBuffer &bank,
                                   const Ledger::Transaction &tx,
                                   uint64_t blockId) const {
  auto genesisRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_GENESIS);
  if (!genesisRoe) {
    return genesisRoe;
  }
  if (tx.fee > 0) {
    auto feeRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_FEE);
    if (!feeRoe) {
      return feeRoe;
    }
  }
  auto &h = txHandlers_[Ledger::Transaction::T_RENEWAL];
  if (!h) {
    return Error(E_INTERNAL,
                 "Genesis renewal transaction handler not registered");
  }
  return mapTxVoid(
      h->applyGenesisRenewal(tx, txContext_, bank, blockId, true, true));
}

Chain::Roe<void>
Chain::processBufferUserEnd(AccountBuffer &bank,
                            const Ledger::Transaction &tx) const {
  auto fromRoe = ensureAccountInBuffer(bank, tx.fromWalletId);
  if (!fromRoe) {
    return fromRoe;
  }
  auto recycleRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_RECYCLE);
  if (!recycleRoe) {
    return recycleRoe;
  }
  auto &h = txHandlers_[Ledger::Transaction::T_END_USER];
  if (!h) {
    return Error(E_INTERNAL, "End-user transaction handler not registered");
  }
  return mapTxVoid(h->applyEndUser(tx, txContext_, bank, true));
}

Chain::Roe<void> Chain::processBufferTx(AccountBuffer &bank,
                                        const Ledger::Transaction &tx) const {
  // All transactions happen in bank; accounts sourced from txContext_.bank on
  // demand
  auto fromRoe = ensureAccountInBuffer(bank, tx.fromWalletId);
  if (!fromRoe) {
    return fromRoe;
  }
  auto toRoe = ensureAccountInBuffer(bank, tx.toWalletId);
  if (!toRoe) {
    return toRoe;
  }
  if (tx.fee > 0) {
    auto feeRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_FEE);
    if (!feeRoe) {
      return feeRoe;
    }
  }
  auto &h = txHandlers_[Ledger::Transaction::T_DEFAULT];
  if (!h) {
    return Error(E_INTERNAL, "Default transaction handler not registered");
  }
  return mapTxVoid(h->applyDefaultTransferStrict(tx, txContext_, bank));
}

Chain::Roe<void> Chain::processTx(const Ledger::Transaction &tx,
                                  uint64_t blockId, bool isStrictMode) {
  (void)blockId;
  log().info << "Processing user transaction";

  auto &h = txHandlers_[Ledger::Transaction::T_DEFAULT];
  if (!h) {
    return Error(E_INTERNAL, "Default transaction handler not registered");
  }
  if (isStrictMode) {
    return mapTxVoid(
        h->applyDefaultTransferStrict(tx, txContext_, txContext_.bank));
  }
  return mapTxVoid(
      h->applyDefaultTransferLoose(tx, txContext_, txContext_.bank));
}

} // namespace pp
