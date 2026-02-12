#include "Chain.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <set>

namespace pp {

std::ostream &operator<<(std::ostream &os,
                         const Chain::CheckpointConfig &config) {
  os << "CheckpointConfig{minBlocks: " << config.minBlocks
     << ", minAgeSeconds: " << config.minAgeSeconds << "}";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const Chain::BlockChainConfig &config) {
  os << "BlockChainConfig{genesisTime: " << config.genesisTime << ", "
     << "slotDuration: " << config.slotDuration << ", "
     << "slotsPerEpoch: " << config.slotsPerEpoch << ", "
     << "maxPendingTransactions: " << config.maxPendingTransactions << ", "
     << "maxTransactionsPerBlock: " << config.maxTransactionsPerBlock << ", "
     << "minFeePerTransaction: " << config.minFeePerTransaction << ", "
     << "checkpoint: " << config.checkpoint << "}";
  return os;
}

std::string Chain::GenesisAccountMeta::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar &VERSION &*this;
  return oss.str();
}

bool Chain::GenesisAccountMeta::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar &version;
  if (version != VERSION) {
    return false;
  }
  ar &*this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

Chain::Chain() {
  redirectLogger("Chain");
  ledger_.redirectLogger(log().getFullName() + ".Ledger");
  consensus_.redirectLogger(log().getFullName() + ".Obo");
}

bool Chain::isStakeholderSlotLeader(uint64_t stakeholderId,
                                    uint64_t slot) const {
  return consensus_.isSlotLeader(slot, stakeholderId);
}

bool Chain::isSlotBlockProductionTime(uint64_t slot) const {
  return consensus_.isSlotBlockProductionTime(slot);
}

bool Chain::isValidSlotLeader(const Ledger::ChainNode &block) const {
  return consensus_.isSlotLeader(block.block.slot, block.block.slotLeader);
}

bool Chain::isValidTimestamp(const Ledger::ChainNode &block) const {
  int64_t slotStartTime = consensus_.getSlotStartTime(block.block.slot);
  int64_t slotEndTime = consensus_.getSlotEndTime(block.block.slot);

  int64_t blockTime = block.block.timestamp;

  if (blockTime < slotStartTime || blockTime > slotEndTime) {
    log().warning << "Block timestamp out of slot range";
    return false;
  }

  return true;
}

bool Chain::isValidBlockSequence(const Ledger::ChainNode &block) const {
  if (block.block.index != ledger_.getNextBlockId()) {
    log().warning << "Invalid block index: expected "
                  << ledger_.getNextBlockId() << " got " << block.block.index;
    return false;
  }

  if (block.block.index == 0) {
    return true;
  }

  auto latestBlockResult = ledger_.readBlock(block.block.index - 1);
  if (!latestBlockResult) {
    log().warning << "Latest block not found: " << block.block.index - 1;
    return false;
  }
  auto latestBlock = latestBlockResult.value();

  if (block.block.index != latestBlock.block.index + 1) {
    log().warning << "Invalid block index: expected "
                  << (latestBlock.block.index + 1) << " got "
                  << block.block.index;
    return false;
  }

  // Check previous hash matches
  if (block.block.previousHash != latestBlock.hash) {
    log().warning << "Invalid previous hash";
    return false;
  }

  return true;
}

bool Chain::needsCheckpoint(const CheckpointConfig &checkpointConfig) const {
  if (getNextBlockId() < currentCheckpointId_ + checkpointConfig.minBlocks) {
    return false;
  }
  if (getBlockAgeSeconds(currentCheckpointId_) <
      checkpointConfig.minAgeSeconds) {
    return false;
  }
  return true;
}

uint64_t Chain::getLastCheckpointId() const { return lastCheckpointId_; }

uint64_t Chain::getCurrentCheckpointId() const { return currentCheckpointId_; }

uint64_t Chain::getNextBlockId() const { return ledger_.getNextBlockId(); }

int64_t Chain::getConsensusTimestamp() const {
  return consensus_.getTimestamp();
}

uint64_t Chain::getCurrentSlot() const { return consensus_.getCurrentSlot(); }

uint64_t Chain::getCurrentEpoch() const { return consensus_.getCurrentEpoch(); }

uint64_t Chain::getTotalStake() const { return consensus_.getTotalStake(); }

uint64_t Chain::getStakeholderStake(uint64_t stakeholderId) const {
  return consensus_.getStake(stakeholderId);
}

Chain::Roe<uint64_t> Chain::getSlotLeader(uint64_t slot) const {
  auto result = consensus_.getSlotLeader(slot);
  if (!result) {
    return Error(E_CONSENSUS_QUERY,
                 "Failed to get slot leader: " + result.error().message);
  }
  return result.value();
}

std::vector<consensus::Stakeholder> Chain::getStakeholders() const {
  return consensus_.getStakeholders();
}

Chain::Roe<Ledger::ChainNode> Chain::getBlock(uint64_t blockId) const {
  auto result = ledger_.readBlock(blockId);
  if (!result) {
    return Error(E_BLOCK_NOT_FOUND,
                 "Block not found: " + std::to_string(blockId));
  }
  return result.value();
}

Chain::Roe<Client::UserAccount> Chain::getAccount(uint64_t accountId) const {
  auto roeAccount = bank_.getAccount(accountId);
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
  auto blockResult = ledger_.readBlock(blockId);
  if (!blockResult) {
    return 0;
  }
  auto block = blockResult.value();

  auto currentTime = consensus_.getTimestamp();
  int64_t blockTime = block.block.timestamp;

  if (currentTime > blockTime) {
    return static_cast<uint64_t>(currentTime - blockTime);
  }

  return 0;
}

Chain::Roe<std::string>
Chain::findAccountMetadataInBlock(const Ledger::Block &block,
                                  const AccountBuffer::Account &account) const {
  const uint64_t accountId = account.id;

  auto unwrapMeta = [](const Chain::Roe<std::string> &metaResult,
                       bool errorAsMessage) -> Chain::Roe<std::string> {
    if (!metaResult) {
      if (errorAsMessage) {
        return metaResult.error().message;
      }
      return metaResult.error();
    }
    return metaResult.value();
  };

  auto matchesAccount = [&](const Ledger::Transaction &tx) -> bool {
    switch (tx.type) {
    case Ledger::Transaction::T_GENESIS:
      // GENESIS record only happens at first block.
      return accountId == AccountBuffer::ID_GENESIS && block.index == 0;
    case Ledger::Transaction::T_CONFIG:
      // CONFIG record only happens with genesis account.
      return accountId == AccountBuffer::ID_GENESIS;
    case Ledger::Transaction::T_NEW_USER:
      // NEW_USER record only happens for non-genesis accounts.
      return accountId != AccountBuffer::ID_GENESIS &&
             tx.toWalletId == accountId;
    case Ledger::Transaction::T_USER:
      // User update must be from the account itself, and cannot be for genesis
      // account (which is only updated by system transactions)
      return accountId != AccountBuffer::ID_GENESIS &&
             tx.fromWalletId == accountId && tx.toWalletId == accountId;
    case Ledger::Transaction::T_RENEWAL:
      return tx.fromWalletId == accountId;
    default:
      return false;
    }
  };

  for (auto it = block.signedTxes.rbegin(); it != block.signedTxes.rend();
       ++it) {
    const auto &tx = it->obj;
    if (!matchesAccount(tx)) {
      continue;
    }

    switch (tx.type) {
    case Ledger::Transaction::T_GENESIS:
      return unwrapMeta(updateMetaFromSystemInit(tx.meta), true);
    case Ledger::Transaction::T_NEW_USER:
      return unwrapMeta(updateMetaFromUserInit(tx.meta, account), false);
    case Ledger::Transaction::T_CONFIG:
      return unwrapMeta(updateMetaFromSystemUpdate(tx.meta), true);
    case Ledger::Transaction::T_USER:
      return unwrapMeta(updateMetaFromUserUpdate(tx.meta, account), false);
    case Ledger::Transaction::T_RENEWAL:
      return unwrapMeta(updateMetaFromUserRenewal(tx.meta, account), false);
    // T_USER_END is not expected to update account metadata, and should not be
    // used as source for account meta, so we skip it here.
    default:
      break;
    }
  }

  return Error(E_INTERNAL,
               "No prior checkpoint/user/renewal from this account in block");
}

Chain::Roe<Ledger::SignedData<Ledger::Transaction>>
Chain::createRenewalTransaction(uint64_t accountId, uint64_t minFee) const {
  auto accountResult = bank_.getAccount(accountId);
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
  tx.fee = static_cast<int64_t>(minFee);

  if (accountId != AccountBuffer::ID_GENESIS) {
    auto balance = bank_.getBalance(accountId, AccountBuffer::ID_GENESIS);
    if (balance < minFee) {
      // Insufficient balance for renewal, terminate account with whatever
      // balance remains Notice the fee is 0 here, all remaining balances will
      // be transferred to the recycle account.
      tx.type = Ledger::Transaction::T_END_USER;
      tx.fee = 0;
    }
  }

  if (tx.type == Ledger::Transaction::T_RENEWAL) {
    // Get account metadata from previous block
    auto blockResult = ledger_.readBlock(account.blockId);
    if (!blockResult) {
      return Error(E_BLOCK_NOT_FOUND,
                   "Block not found: " + std::to_string(account.blockId));
    }
    auto const &block = blockResult.value().block;

    auto metaResult = findAccountMetadataInBlock(block, account);
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
         bank_.getAccountIdsBeforeBlockId(maxBlockIdForRenewal)) {
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
      auto accountResult = bank_.getAccount(accountId);
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
  const uint64_t minBlocks = chainConfig_.checkpoint.minBlocks;
  if (atBlockId < minBlocks) {
    return 0;
  }
  uint64_t maxBlockIdFromBlocks = atBlockId - minBlocks + 1;

  const uint64_t minAgeSeconds = chainConfig_.checkpoint.minAgeSeconds;

  uint64_t maxBlockIdFromTime = atBlockId;
  if (minAgeSeconds > 0 && atBlockId > 0) {
    const int64_t cutoffTimestamp =
        getConsensusTimestamp() - static_cast<int64_t>(minAgeSeconds);
    auto roeBlock = ledger_.findBlockByTimestamp(cutoffTimestamp);
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
  const uint64_t nextBlockId = ledger_.getNextBlockId();

  auto maxBlockIdResult = calculateMaxBlockIdForRenewal(nextBlockId);
  if (!maxBlockIdResult) {
    return maxBlockIdResult.error();
  }
  const uint64_t maxBlockIdForRenewal = maxBlockIdResult.value();
  if (maxBlockIdForRenewal == 0) {
    return renewals;
  }

  const uint64_t minFee = chainConfig_.minFeePerTransaction;
  for (uint64_t accountId :
       bank_.getAccountIdsBeforeBlockId(maxBlockIdForRenewal)) {
    auto renewalResult = createRenewalTransaction(accountId, minFee);
    if (!renewalResult) {
      return renewalResult.error();
    }
    renewals.push_back(renewalResult.value());
  }

  return renewals;
}

Chain::Roe<Ledger::ChainNode> Chain::readLastBlock() const {
  auto result = ledger_.readLastBlock();
  if (!result) {
    return Error(E_LEDGER_READ,
                 "Failed to read last block: " + result.error().message);
  }
  return result.value();
}

std::string Chain::calculateHash(const Ledger::Block &block) const {
  // Use ltsToString() to get the serialized block representation
  std::string serialized = block.ltsToString();
  return utl::sha256(serialized);
}

void Chain::refreshStakeholders() {
  if (consensus_.isStakeUpdateNeeded()) {
    auto stakeholders = bank_.getStakeholders();
    consensus_.setStakeholders(stakeholders);
  }
}

void Chain::initConsensus(const consensus::Ouroboros::Config &config) {
  consensus_.init(config);
}

Chain::Roe<void> Chain::initLedger(const Ledger::InitConfig &config) {
  auto result = ledger_.init(config);
  if (!result) {
    return Error(E_STATE_INIT,
                 "Failed to initialize ledger: " + result.error().message);
  }
  return {};
}

Chain::Roe<void> Chain::mountLedger(const std::string &workDir) {
  auto result = ledger_.mount(workDir);
  if (!result) {
    return Error(E_STATE_MOUNT,
                 "Failed to mount ledger: " + result.error().message);
  }
  return {};
}

Chain::Roe<uint64_t> Chain::loadFromLedger(uint64_t startingBlockId) {
  log().info << "Loading from ledger starting at block ID " << startingBlockId;

  log().info << "Resetting account buffer";
  bank_.reset();

  // Process blocks from ledger one by one
  uint64_t blockId = startingBlockId;
  uint64_t logInterval = 1000; // Log every 1000 blocks
  bool isStrictMode =
      startingBlockId ==
      0; // True if we are loading from the beginning (strict validation)
  while (true) {
    auto blockResult = ledger_.readBlock(blockId);
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
  const uint64_t minFeePerTransaction = gm.config.minFeePerTransaction;

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
  if (feeTx.obj.fee != 0) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis fee account creation transaction must have fee 0");
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
  if (minerTx.obj.amount + minerTx.obj.fee !=
      AccountBuffer::INITIAL_TOKEN_SUPPLY) {
    return Error(E_BLOCK_GENESIS,
                 "Genesis miner transaction must have amount + fee: " +
                     std::to_string(AccountBuffer::INITIAL_TOKEN_SUPPLY));
  }

  // Fourth transaction: recycle account creation (ID_GENESIS -> ID_RECYCLE, 0)
  const auto &recycleTx = block.block.signedTxes[3];
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
  if (recycleTx.obj.fee != static_cast<int64_t>(minFeePerTransaction)) {
    return Error(
        E_BLOCK_GENESIS,
        "Genesis recycle account creation transaction must have fee: " +
            std::to_string(minFeePerTransaction));
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

Chain::Roe<void>
Chain::validateNormalBlock(const Ledger::ChainNode &block) const {
  // Non-genesis: validate slot leader and timing
  uint64_t slot = block.block.slot;
  uint64_t slotLeader = block.block.slotLeader;
  if (!consensus_.validateSlotLeader(slotLeader, slot)) {
    return Error(E_CONSENSUS_SLOT_LEADER,
                 "Invalid slot leader for block at slot " +
                     std::to_string(slot));
  }
  if (!consensus_.validateBlockTiming(block.block.timestamp, slot)) {
    return Error(E_CONSENSUS_TIMING,
                 "Block timestamp outside valid slot range");
  }

  // Validate hash chain (previous block link and index)
  if (block.block.index > 0) {
    auto latestBlockResult = ledger_.readBlock(block.block.index - 1);
    if (!latestBlockResult) {
      return Error(E_BLOCK_NOT_FOUND,
                   "Latest block not found: " +
                       std::to_string(block.block.index - 1));
    }
    auto latestBlock = latestBlockResult.value();
    if (block.block.previousHash != latestBlock.hash) {
      return Error(E_BLOCK_CHAIN, "Block previous hash does not match chain");
    }
    if (block.block.index != latestBlock.block.index + 1) {
      return Error(E_BLOCK_INDEX, "Block index mismatch");
    }
  }

  // Validate block hash
  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
    return Error(E_BLOCK_HASH, "Block hash validation failed");
  }

  // Validate sequence
  if (!isValidBlockSequence(block)) {
    return Error(E_BLOCK_SEQUENCE, "Invalid block sequence");
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

  return {};
}

Chain::Roe<std::string>
Chain::updateMetaFromSystemInit(const std::string &meta) const {
  return updateSystemMeta(meta);
}

Chain::Roe<std::string>
Chain::updateMetaFromSystemUpdate(const std::string &meta) const {
  return updateSystemMeta(meta);
}

Chain::Roe<std::string> Chain::updateSystemMeta(const std::string &meta) const {
  GenesisAccountMeta gm;
  if (!gm.ltsFromString(meta)) {
    return Error(E_INTERNAL_DESERIALIZE, "Failed to deserialize checkpoint: " +
                                             std::to_string(meta.size()) +
                                             " bytes");
  }

  auto genesisAccountResult = bank_.getAccount(AccountBuffer::ID_GENESIS);
  if (!genesisAccountResult) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "Account not found: " +
                     std::to_string(AccountBuffer::ID_GENESIS));
  }
  auto const &genesisAccount = genesisAccountResult.value();
  gm.genesis.wallet = genesisAccount.wallet;
  return gm.ltsToString();
}

Chain::Roe<std::string>
Chain::updateMetaFromUserInit(const std::string &meta,
                              const AccountBuffer::Account &account) const {
  return updateUserMeta(meta, account);
}

Chain::Roe<std::string>
Chain::updateMetaFromUserUpdate(const std::string &meta,
                                const AccountBuffer::Account &account) const {
  return updateUserMeta(meta, account);
}

Chain::Roe<std::string>
Chain::updateMetaFromUserRenewal(const std::string &meta,
                                 const AccountBuffer::Account &account) const {
  return updateUserMeta(meta, account);
}

Chain::Roe<std::string>
Chain::updateUserMeta(const std::string &meta,
                      const AccountBuffer::Account &account) const {
  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(meta)) {
    return Error(E_INTERNAL_DESERIALIZE,
                 "Failed to deserialize account info: " +
                     std::to_string(meta.size()) + " bytes");
  }
  userAccount.wallet = account.wallet;
  return userAccount.ltsToString();
}

Chain::Roe<void> Chain::addBlock(const Ledger::ChainNode &block,
                                 bool isStrictMode) {
  auto processResult = processBlock(block, isStrictMode);
  if (!processResult) {
    return Error(E_BLOCK_VALIDATION,
                 "Failed to process block: " + processResult.error().message);
  }

  auto ledgerResult = ledger_.addBlock(block);
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
  auto roe = validateNormalBlock(block);
  if (!roe) {
    return Error(E_BLOCK_VALIDATION, "Block validation failed for block " +
                                         std::to_string(block.block.index) +
                                         ": " + roe.error().message);
  }

  // Process checkpoint transactions to restore BlockChainConfig
  for (const auto &signedTx : block.block.signedTxes) {
    auto result = processNormalTxRecord(signedTx, block.block.index,
                                        block.block.slotLeader, isStrictMode);
    if (!result) {
      return Error(E_TX_VALIDATION,
                   "Failed to process transaction: " + result.error().message);
    }
  }

  return {};
}

Chain::Roe<void> Chain::addBufferTransaction(
    AccountBuffer &bank,
    const Ledger::SignedData<Ledger::Transaction> &signedTx,
    uint64_t slotLeaderId) const {
  auto roe = validateTxSignatures(signedTx, slotLeaderId, true);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  const auto &tx = signedTx.obj;
  switch (tx.type) {
  case Ledger::Transaction::T_DEFAULT:
    return processBufferTransaction(bank, tx);
  case Ledger::Transaction::T_NEW_USER:
    return processBufferUserInit(bank, tx);
  case Ledger::Transaction::T_CONFIG:
    return processBufferSystemUpdate(bank, tx);
  case Ledger::Transaction::T_USER:
  case Ledger::Transaction::T_RENEWAL:
    return processBufferUserAccountUpsert(bank, tx);
  case Ledger::Transaction::T_END_USER:
    return processBufferUserEnd(bank, tx);
  default:
    return Error(E_TX_TYPE, "Unknown transaction type: " +
                                std::to_string(tx.type));
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
  case Ledger::Transaction::T_GENESIS:
    return processSystemInit(tx);
  case Ledger::Transaction::T_NEW_USER:
    return processUserInit(tx, 0);
  default:
    return Error(E_TX_TYPE, "Unknown transaction type in genesis block: " +
                                std::to_string(tx.type));
  }
}

Chain::Roe<void> Chain::processNormalTxRecord(
    const Ledger::SignedData<Ledger::Transaction> &signedTx, uint64_t blockId,
    uint64_t slotLeaderId, bool isStrictMode) {
  auto roe = validateTxSignatures(signedTx, slotLeaderId, isStrictMode);
  if (!roe) {
    return Error(E_TX_SIGNATURE,
                 "Failed to validate transaction: " + roe.error().message);
  }

  auto const &tx = signedTx.obj;
  switch (tx.type) {
  case Ledger::Transaction::T_NEW_USER:
    return processUserInit(tx, blockId);
  case Ledger::Transaction::T_CONFIG:
    return processSystemUpdate(tx, blockId, isStrictMode);
  case Ledger::Transaction::T_USER:
    return processUserUpdate(tx, blockId, isStrictMode);
  case Ledger::Transaction::T_RENEWAL:
    return processUserRenewal(tx, blockId, isStrictMode);
  case Ledger::Transaction::T_END_USER:
    return processUserEnd(tx, blockId, isStrictMode);
  case Ledger::Transaction::T_DEFAULT:
    return processTransaction(tx, blockId, isStrictMode);
  default:
    return Error(E_TX_TYPE,
                 "Unknown transaction type: " + std::to_string(tx.type));
  }
}

Chain::Roe<void> Chain::verifySignaturesAgainstAccount(
    const Ledger::Transaction &tx, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account) const {
  if (signatures.size() < account.wallet.minSignatures) {
    return Error(
        E_TX_SIGNATURE,
        "Account " + std::to_string(account.id) + " must have at least " +
            std::to_string(account.wallet.minSignatures) +
            " signatures, but has " + std::to_string(signatures.size()));
  }
  auto message = utl::binaryPack(tx);
  std::vector<bool> keyUsed(account.wallet.publicKeys.size(), false);
  for (const auto &signature : signatures) {
    bool matched = false;
    for (size_t i = 0; i < account.wallet.publicKeys.size(); ++i) {
      if (keyUsed[i])
        continue;
      const auto &publicKey = account.wallet.publicKeys[i];
      if (utl::ed25519Verify(publicKey, message, signature)) {
        keyUsed[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      log().error << "Invalid signature for account " +
                         std::to_string(account.id) + ": " +
                         utl::toJsonSafeString(signature);
      log().error << "Expected signatures: " << account.wallet.minSignatures;
      for (size_t i = 0; i < account.wallet.publicKeys.size(); ++i) {
        log().error << "Public key " << i << ": "
                    << utl::toJsonSafeString(account.wallet.publicKeys[i]);
        log().error << "Key used: " << keyUsed[i];
      }
      for (const auto &sig : signatures) {
        log().error << "Signature: " << utl::toJsonSafeString(sig);
      }
      return Error(E_TX_SIGNATURE,
                   "Invalid or duplicate signature for account " +
                       std::to_string(account.id));
    }
  }
  return {};
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

  auto accountResult = bank_.getAccount(signerAccountId);
  if (!accountResult) {
    if (isStrictMode) {
      if (bank_.isEmpty() && signerAccountId == AccountBuffer::ID_GENESIS) {
        // Genesis account is created by the system checkpoint, this is not very
        // good way of handling Should avoid using this generic handlers for
        // specific case
        return {};
      }
      return Error(E_ACCOUNT_NOT_FOUND,
                   "Failed to get account: " + accountResult.error().message);
    } else {
      // In loose mode, account may not be created before their transactions
      return {};
    }
  }
  return verifySignaturesAgainstAccount(tx, signedTx.signatures,
                                        accountResult.value());
}

Chain::Roe<void> Chain::processSystemInit(const Ledger::Transaction &tx) {
  log().info << "Processing system initialization transaction";

  if (tx.fromWalletId != AccountBuffer::ID_GENESIS ||
      tx.toWalletId != AccountBuffer::ID_GENESIS) {
    return Error(E_TX_VALIDATION, "System init transaction must use genesis "
                                  "wallet (ID_GENESIS -> ID_GENESIS)");
  }
  if (tx.amount != 0) {
    return Error(E_TX_VALIDATION, "System init transaction must have amount 0");
  }
  if (tx.fee != 0) {
    return Error(E_TX_VALIDATION, "System init transaction must have fee 0");
  }

  // Deserialize BlockChainConfig from transaction metadata
  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.meta)) {
    return Error(E_INTERNAL_DESERIALIZE,
                 "Failed to deserialize checkpoint config: " + tx.meta);
  }

  // Reset chain configuration
  chainConfig_ = gm.config;

  // Reset consensus parameters
  auto config = consensus_.getConfig();

  if (config.genesisTime == 0) {
    config.genesisTime = chainConfig_.genesisTime;
  } else if (chainConfig_.genesisTime != config.genesisTime) {
    return Error(E_TX_VALIDATION, "Genesis time mismatch");
  }

  config.slotDuration = chainConfig_.slotDuration;
  config.slotsPerEpoch = chainConfig_.slotsPerEpoch;
  consensus_.init(config);

  AccountBuffer::Account genesisAccount;
  genesisAccount.id = AccountBuffer::ID_GENESIS;
  genesisAccount.wallet = gm.genesis.wallet;
  auto roeAddGenesis = bank_.add(genesisAccount);
  if (!roeAddGenesis) {
    return Error(E_INTERNAL_BUFFER,
                 "Failed to add genesis account to buffer: " +
                     roeAddGenesis.error().message);
  }

  log().info << "System initialized";
  log().info << "  Version: " << gm.VERSION;
  log().info << "  Config: " << chainConfig_;
  log().info << "  Genesis: " << gm.genesis;

  return {};
}

Chain::Roe<Chain::GenesisAccountMeta> Chain::processSystemUpdateImpl(
    AccountBuffer &bank, const Ledger::Transaction &tx) const {
  if (tx.fromWalletId != AccountBuffer::ID_GENESIS ||
      tx.toWalletId != AccountBuffer::ID_GENESIS) {
    return Error(E_TX_VALIDATION, "System update transaction must use genesis "
                                  "wallet (ID_GENESIS -> ID_GENESIS)");
  }
  if (tx.amount != 0) {
    return Error(E_TX_VALIDATION,
                 "System update transaction must have amount 0");
  }
  if (tx.fee != 0) {
    return Error(E_TX_VALIDATION, "System update transaction must have fee 0");
  }

  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.meta)) {
    return Error(E_INTERNAL_DESERIALIZE,
                 "Failed to deserialize checkpoint config: " + tx.meta);
  }

  if (gm.config.genesisTime != chainConfig_.genesisTime) {
    return Error(E_TX_VALIDATION, "Genesis time mismatch");
  }

  if (gm.config.slotDuration > chainConfig_.slotDuration) {
    return Error(E_TX_VALIDATION, "Slot duration cannot be increased");
  }

  if (gm.config.slotsPerEpoch < chainConfig_.slotsPerEpoch) {
    return Error(E_TX_VALIDATION, "Slots per epoch cannot be decreased");
  }

  if (gm.genesis.wallet.publicKeys.size() < 3) {
    return Error(E_TX_VALIDATION,
                 "Genesis account must have at least 3 public keys");
  }

  if (gm.genesis.wallet.minSignatures < 2) {
    return Error(E_TX_VALIDATION,
                 "Genesis account must have at least 2 signatures");
  }

  if (!bank.verifyBalance(AccountBuffer::ID_GENESIS, 0, 0,
                          gm.genesis.wallet.mBalances)) {
    return Error(E_TX_VALIDATION, "Genesis account balance mismatch");
  }

  return gm;
}

Chain::Roe<void> Chain::processSystemUpdate(const Ledger::Transaction &tx,
                                            uint64_t blockId,
                                            bool isStrictMode) {
  log().info << "Processing system update transaction";
  auto result = processSystemUpdateImpl(bank_, tx);
  if (!result) {
    return result.error();
  }
  chainConfig_ = result.value().config;
  log().info << "System updated";
  log().info << "  Version: " << GenesisAccountMeta::VERSION;
  log().info << "  Config: " << chainConfig_;
  return {};
}

Chain::Roe<void> Chain::processUserInitImpl(AccountBuffer &bank,
                                             const Ledger::Transaction &tx,
                                             uint64_t blockId,
                                             bool isBufferMode) const {
  if (tx.fee < chainConfig_.minFeePerTransaction) {
    return Error(E_TX_FEE, "New user transaction fee below minimum: " +
                               std::to_string(tx.fee));
  }

  bool toWalletExists =
      bank.hasAccount(tx.toWalletId) ||
      (isBufferMode && bank_.hasAccount(tx.toWalletId));
  if (toWalletExists) {
    return Error(E_ACCOUNT_EXISTS,
                 "Account already exists: " + std::to_string(tx.toWalletId));
  }

  if (isBufferMode) {
    auto roe = ensureAccountInBuffer(bank, tx.fromWalletId);
    if (!roe) {
      return roe;
    }
  }

  auto spendingResult =
      bank.verifySpendingPower(tx.fromWalletId, AccountBuffer::ID_GENESIS,
                               tx.amount, tx.fee);
  if (!spendingResult) {
    return Error(E_ACCOUNT_BALANCE,
                 "Source account must have sufficient balance: " +
                     spendingResult.error().message);
  }

  if (tx.fromWalletId != AccountBuffer::ID_GENESIS &&
      tx.toWalletId < AccountBuffer::ID_FIRST_USER) {
    return Error(E_TX_VALIDATION,
                 "New user account id must be larger than: " +
                     std::to_string(AccountBuffer::ID_FIRST_USER));
  }

  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(tx.meta)) {
    return Error(E_INTERNAL_DESERIALIZE,
                 "Failed to deserialize user account: " + tx.meta);
  }

  if (userAccount.wallet.publicKeys.empty()) {
    return Error(E_TX_VALIDATION,
                 "User account must have at least one public key");
  }
  if (userAccount.wallet.minSignatures < 1) {
    return Error(E_TX_VALIDATION,
                 "User account must require at least one signature");
  }
  if (userAccount.wallet.mBalances.size() != 1) {
    return Error(E_TX_VALIDATION, "User account must have exactly one balance");
  }
  auto it = userAccount.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
  if (it == userAccount.wallet.mBalances.end()) {
    return Error(E_TX_VALIDATION,
                 "User account must have balance in ID_GENESIS token");
  }
  if (it->second != tx.amount) {
    return Error(E_TX_VALIDATION,
                 "User account must have balance in ID_GENESIS token: " +
                     std::to_string(it->second));
  }

  AccountBuffer::Account account;
  account.id = tx.toWalletId;
  account.blockId = blockId;
  account.wallet = userAccount.wallet;
  account.wallet.mBalances.clear();

  auto addResult = bank.add(account);
  if (!addResult) {
    return Error(E_INTERNAL_BUFFER, "Failed to add user account to buffer: " +
                                        addResult.error().message);
  }

  auto transferResult = bank.transferBalance(
      tx.fromWalletId, tx.toWalletId, AccountBuffer::ID_GENESIS, tx.amount);
  if (!transferResult) {
    return Error(E_TX_TRANSFER, "Failed to transfer balance: " +
                                    transferResult.error().message);
  }

  return {};
}

Chain::Roe<void> Chain::processUserInit(const Ledger::Transaction &tx,
                                        uint64_t blockId) {
  log().info << "Processing user initialization transaction";
  auto result = processUserInitImpl(bank_, tx, blockId, false);
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

Chain::Roe<void> Chain::processUserAccountUpsertImpl(
    AccountBuffer &bank, const Ledger::Transaction &tx, uint64_t blockId,
    bool isBufferMode, bool isStrictMode) const {
  if (tx.tokenId != AccountBuffer::ID_GENESIS) {
    return Error(E_TX_VALIDATION,
                 "User update transaction must use genesis token (ID_GENESIS)");
  }

  if (tx.fromWalletId != tx.toWalletId) {
    return Error(E_TX_VALIDATION,
                 "User update transaction must use same from and to wallet IDs");
  }

  if (tx.fee < chainConfig_.minFeePerTransaction) {
    return Error(E_TX_FEE, "User update transaction fee below minimum: " +
                               std::to_string(tx.fee));
  }

  if (tx.amount != 0) {
    return Error(E_TX_VALIDATION, "User update transaction must have amount 0");
  }

  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(tx.meta)) {
    return Error(E_INTERNAL_DESERIALIZE, "Failed to deserialize user meta: " +
                                             std::to_string(tx.meta.size()) +
                                             " bytes");
  }

  if (userAccount.wallet.publicKeys.empty()) {
    return Error(E_TX_VALIDATION,
                 "User account must have at least one public key");
  }

  if (userAccount.wallet.minSignatures < 1) {
    return Error(E_TX_VALIDATION,
                 "User account must require at least one signature");
  }

  if (isBufferMode) {
    auto roe = ensureAccountInBuffer(bank, tx.fromWalletId);
    if (!roe) {
      return roe;
    }
  }

  auto bufferAccountResult = bank.getAccount(tx.fromWalletId);
  if (!bufferAccountResult) {
    if (isStrictMode) {
      return Error(E_ACCOUNT_NOT_FOUND, "User account not found in buffer: " +
                                            std::to_string(tx.fromWalletId));
    }
  } else {
    auto balanceVerifyResult =
        bank.verifyBalance(tx.fromWalletId, 0, tx.fee,
                          userAccount.wallet.mBalances);
    if (!balanceVerifyResult) {
      return Error(E_TX_VALIDATION, balanceVerifyResult.error().message);
    }
  }

  bank.remove(tx.fromWalletId);

  AccountBuffer::Account account;
  account.id = tx.fromWalletId;
  account.blockId = blockId;
  account.wallet = userAccount.wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return Error(E_INTERNAL_BUFFER, "Failed to add user account to buffer: " +
                                        addResult.error().message);
  }

  return {};
}

Chain::Roe<void> Chain::processUserAccountUpsert(const Ledger::Transaction &tx,
                                                 uint64_t blockId,
                                                 bool isStrictMode) {
  log().info << "Processing user update/renewal transaction";
  auto result =
      processUserAccountUpsertImpl(bank_, tx, blockId, false, isStrictMode);
  if (!result) {
    return result.error();
  }
  log().info << "User account " << tx.fromWalletId << " updated";
  return {};
}

Chain::Roe<void> Chain::processUserEndImpl(AccountBuffer &bank,
                                           const Ledger::Transaction &tx,
                                           bool isBufferMode) const {
  if (tx.tokenId != AccountBuffer::ID_GENESIS) {
    return Error(E_TX_VALIDATION,
                 "User end transaction must use genesis token (ID_GENESIS)");
  }

  if (tx.fromWalletId != tx.toWalletId) {
    return Error(E_TX_VALIDATION,
                 "User end transaction must use same from and to wallet IDs");
  }

  if (tx.amount != 0) {
    return Error(E_TX_VALIDATION, "User end transaction must have amount 0");
  }

  if (tx.fee != 0) {
    return Error(E_TX_VALIDATION, "User end transaction must have fee 0");
  }

  if (isBufferMode) {
    auto accountRoe = ensureAccountInBuffer(bank, tx.fromWalletId);
    if (!accountRoe) {
      return accountRoe;
    }
    auto recycleRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_RECYCLE);
    if (!recycleRoe) {
      return recycleRoe;
    }
  }

  if (!bank.hasAccount(tx.fromWalletId)) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "User account not found: " + std::to_string(tx.fromWalletId));
  }

  if (bank.getBalance(tx.fromWalletId, AccountBuffer::ID_GENESIS) >=
      chainConfig_.minFeePerTransaction) {
    return Error(E_TX_VALIDATION,
                 "User account must have less than " +
                     std::to_string(chainConfig_.minFeePerTransaction) +
                     " balance in ID_GENESIS token");
  }

  auto writeOffResult = bank.writeOff(tx.fromWalletId);
  if (!writeOffResult) {
    return Error(E_INTERNAL_BUFFER, "Failed to write off user account: " +
                                        writeOffResult.error().message);
  }

  return {};
}

Chain::Roe<void> Chain::processUserEnd(const Ledger::Transaction &tx,
                                       uint64_t blockId,
                                       bool isStrictMode) {
  log().info << "Processing user end transaction";
  auto result = processUserEndImpl(bank_, tx, false);
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
  if (!bank_.hasAccount(accountId)) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "Account not found: " + std::to_string(accountId));
  }
  auto accountResult = bank_.getAccount(accountId);
  if (!accountResult) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "Failed to get account from bank: " +
                     accountResult.error().message);
  }
  auto addResult = bank.add(accountResult.value());
  if (!addResult) {
    return Error(E_ACCOUNT_BUFFER,
                 "Failed to add account to buffer: " + addResult.error().message);
  }
  return {};
}

Chain::Roe<void>
Chain::processBufferTransaction(AccountBuffer &bank,
                                const Ledger::Transaction &tx) const {
  // All transactions happen in bank; accounts sourced from bank_ on demand
  auto fromRoe = ensureAccountInBuffer(bank, tx.fromWalletId);
  if (!fromRoe) {
    return fromRoe;
  }
  auto toRoe = ensureAccountInBuffer(bank, tx.toWalletId);
  if (!toRoe) {
    return toRoe;
  }
  return strictProcessTransaction(bank, tx);
}

Chain::Roe<void> Chain::processBufferUserInit(AccountBuffer &bank,
                                              const Ledger::Transaction &tx) const {
  return processUserInitImpl(bank, tx, getNextBlockId(), true);
}

Chain::Roe<void> Chain::processBufferSystemUpdate(
    AccountBuffer &bank, const Ledger::Transaction &tx) const {
  auto genesisRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_GENESIS);
  if (!genesisRoe) {
    return genesisRoe;
  }
  auto configResult = processSystemUpdateImpl(bank, tx);
  if (!configResult) {
    return configResult.error();
  }
  return {};
}

Chain::Roe<void> Chain::processBufferUserAccountUpsert(
    AccountBuffer &bank, const Ledger::Transaction &tx) const {
  return processUserAccountUpsertImpl(bank, tx, getNextBlockId(), true, true);
}

Chain::Roe<void> Chain::processBufferUserEnd(AccountBuffer &bank,
                                             const Ledger::Transaction &tx) const {
  return processUserEndImpl(bank, tx, true);
}

Chain::Roe<void> Chain::processTransaction(const Ledger::Transaction &tx,
                                           uint64_t blockId,
                                           bool isStrictMode) {
  log().info << "Processing user transaction";

  if (isStrictMode) {
    return strictProcessTransaction(bank_, tx);
  } else {
    return looseProcessTransaction(tx);
  }
}

Chain::Roe<void>
Chain::strictProcessTransaction(AccountBuffer &bank,
                                const Ledger::Transaction &tx) const {
  if (tx.fee < chainConfig_.minFeePerTransaction) {
    return Error(E_TX_FEE,
                 "Transaction fee below minimum: " + std::to_string(tx.fee));
  }

  auto transferResult = bank.transferBalance(tx.fromWalletId, tx.toWalletId,
                                             tx.tokenId, tx.amount, tx.fee);
  if (!transferResult) {
    return Error(E_TX_TRANSFER,
                 "Transaction failed: " + transferResult.error().message);
  }

  return {};
}

Chain::Roe<void> Chain::looseProcessTransaction(const Ledger::Transaction &tx) {
  // Existing wallets are created by user checkpoints, they have correct
  // balances.
  if (bank_.hasAccount(tx.fromWalletId)) {
    if (bank_.hasAccount(tx.toWalletId)) {
      auto transferResult = bank_.transferBalance(
          tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount);
      if (!transferResult) {
        return Error(E_TX_TRANSFER, "Failed to transfer balance: " +
                                        transferResult.error().message);
      }
    } else {
      // To unknown wallet
      auto withdrawResult =
          bank_.withdrawBalance(tx.fromWalletId, tx.tokenId, tx.amount);
      if (!withdrawResult) {
        return Error(E_TX_TRANSFER, "Failed to withdraw balance: " +
                                        withdrawResult.error().message);
      }
    }
  } else {
    // From unknown wallet
    if (bank_.hasAccount(tx.toWalletId)) {
      auto depositResult =
          bank_.depositBalance(tx.toWalletId, tx.tokenId, tx.amount);
      if (!depositResult) {
        return Error(E_TX_TRANSFER, "Failed to deposit balance: " +
                                        depositResult.error().message);
      }
    } else {
      // From and to unknown wallets, ignore
    }
  }
  return {};
}

} // namespace pp
