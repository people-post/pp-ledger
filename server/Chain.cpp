#include "Chain.h"
#include "lib/common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <set>

namespace pp {

namespace {

constexpr uint64_t BYTES_PER_KIB = 1024ULL;

uint64_t getFeeCoefficient(const std::vector<uint16_t> &coefficients,
                           size_t index) {
  return index < coefficients.size()
             ? static_cast<uint64_t>(coefficients[index])
             : 0ULL;
}

} // namespace

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
     << "maxCustomMetaSize: " << config.maxCustomMetaSize << ", "
     << "maxTransactionsPerBlock: " << config.maxTransactionsPerBlock << ", "
     << "minFeeCoefficients: [" << utl::join(config.minFeeCoefficients, ", ")
     << "], "
     << "freeCustomMetaSize: " << config.freeCustomMetaSize << ", "
     << "checkpoint: " << config.checkpoint << ", "
     << "maxValidationTimespanSeconds: " << config.maxValidationTimespanSeconds
     << "}";
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

Chain::Roe<uint64_t> Chain::calculateMinimumFeeFromNonFreeMetaSize(
    const BlockChainConfig &config, uint64_t nonFreeCustomMetaSizeBytes) const {
  if (config.minFeeCoefficients.empty()) {
    return Error(E_TX_VALIDATION, "minFeeCoefficients must not be empty");
  }
  const uint64_t nonFreeSizeKiB =
      nonFreeCustomMetaSizeBytes == 0
          ? 0ULL
          : (nonFreeCustomMetaSizeBytes + BYTES_PER_KIB - 1) / BYTES_PER_KIB;

  const unsigned __int128 a = getFeeCoefficient(config.minFeeCoefficients, 0);
  const unsigned __int128 b = getFeeCoefficient(config.minFeeCoefficients, 1);
  const unsigned __int128 c = getFeeCoefficient(config.minFeeCoefficients, 2);
  const unsigned __int128 x = nonFreeSizeKiB;
  const unsigned __int128 minimumFee = a + b * x + c * x * x;

  if (minimumFee >
      static_cast<unsigned __int128>(std::numeric_limits<int64_t>::max())) {
    return Error(E_TX_VALIDATION,
                 "Calculated minimum fee exceeds int64_t range");
  }

  return static_cast<uint64_t>(minimumFee);
}

Chain::Roe<size_t>
Chain::extractNonFreeCustomMetaSizeForFee(const BlockChainConfig &config,
                                          const Ledger::Transaction &tx) const {
  auto toNonFree = [&](size_t customMetaSizeBytes) -> Roe<size_t> {
    if (customMetaSizeBytes > config.maxCustomMetaSize) {
      return Error(E_TX_VALIDATION,
                   "Custom metadata exceeds maxCustomMetaSize: " +
                       std::to_string(customMetaSizeBytes) + " > " +
                       std::to_string(config.maxCustomMetaSize));
    }
    if (customMetaSizeBytes <= config.freeCustomMetaSize) {
      return 0;
    }
    return customMetaSizeBytes - config.freeCustomMetaSize;
  };

  if (tx.meta.size() <= config.freeCustomMetaSize) {
    return 0;
  }

  switch (tx.type) {
  case Ledger::Transaction::T_NEW_USER:
  case Ledger::Transaction::T_USER: {
    Client::UserAccount userAccount;
    if (!userAccount.ltsFromString(tx.meta)) {
      return Error(
          E_INTERNAL_DESERIALIZE,
          "Failed to deserialize user account metadata for fee calculation");
    }
    return toNonFree(userAccount.meta.size());
  }
  case Ledger::Transaction::T_RENEWAL: {
    if (tx.fromWalletId == AccountBuffer::ID_GENESIS &&
        tx.toWalletId == AccountBuffer::ID_GENESIS) {
      GenesisAccountMeta gm;
      if (!gm.ltsFromString(tx.meta)) {
        return Error(
            E_INTERNAL_DESERIALIZE,
            "Failed to deserialize genesis metadata for fee calculation");
      }
      return toNonFree(gm.genesis.meta.size());
    }

    Client::UserAccount userAccount;
    if (!userAccount.ltsFromString(tx.meta)) {
      return Error(
          E_INTERNAL_DESERIALIZE,
          "Failed to deserialize renewal metadata for fee calculation");
    }
    return toNonFree(userAccount.meta.size());
  }
  default:
    return toNonFree(tx.meta.size());
  }
}

Chain::Roe<uint64_t>
Chain::calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                         const Ledger::Transaction &tx) const {
  auto nonFreeMetaSizeResult = extractNonFreeCustomMetaSizeForFee(config, tx);
  if (!nonFreeMetaSizeResult) {
    return nonFreeMetaSizeResult.error();
  }
  return calculateMinimumFeeFromNonFreeMetaSize(config,
                                                nonFreeMetaSizeResult.value());
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

bool Chain::isChainConfigReady() const { return optChainConfig_.has_value(); }

bool Chain::shouldUseStrictMode(uint64_t blockIndex) const {
  if (checkpoint_.currentId == 0) {
    return true;
  }
  if (checkpoint_.currentId == checkpoint_.lastId) {
    // Not fully initialized yet
    return false;
  }
  return blockIndex >= checkpoint_.currentId;
}

Chain::Roe<void> Chain::validateBlockSequence(const Ledger::ChainNode &block) const {
  const uint64_t startingBlockId = ledger_.getStartingBlockId();
  if (block.block.index < startingBlockId) {
    return Error(E_BLOCK_INDEX, "Invalid block index: expected >= " + std::to_string(startingBlockId) + " got " + std::to_string(block.block.index));
  }

  const uint64_t nextBlockId = ledger_.getNextBlockId();
  if (block.block.index > nextBlockId) {
    return Error(E_BLOCK_INDEX, "Invalid block index: expected <= " + std::to_string(nextBlockId) + " got " + std::to_string(block.block.index));
  }

  // For the first block in this ledger range, there is no previous block.
  if (block.block.index > startingBlockId) {
    auto prevBlockResult = ledger_.readBlock(block.block.index - 1);
    if (!prevBlockResult) {
      return Error(E_BLOCK_NOT_FOUND, "Latest block not found: " + std::to_string(block.block.index - 1));
    }
    auto prevBlock = prevBlockResult.value();

    if (block.block.index != prevBlock.block.index + 1) {
      return Error(E_BLOCK_INDEX, "Invalid block index: expected " + std::to_string(prevBlock.block.index + 1) + " got " + std::to_string(block.block.index));
    }

    // Check previous hash matches
    if (block.block.previousHash != prevBlock.hash) {
      return Error(E_BLOCK_HASH, "Invalid previous hash: expected " + prevBlock.hash + " got " + block.block.previousHash);
    }

    // txIndex must equal previous block's cumulative transaction count
    const uint64_t expectedTxIndex = prevBlock.block.txIndex + prevBlock.block.signedTxes.size();
    if (block.block.txIndex != expectedTxIndex) {
      return Error(E_BLOCK_INDEX, "Invalid txIndex: expected " + std::to_string(expectedTxIndex) + " got " + std::to_string(block.block.txIndex));
    }
  }

  return {};
}

bool Chain::needsCheckpoint(const BlockChainConfig &config) const {
  // Keep checkpoints at least one epoch beyond the renewal span to avoid
  // edge cases where renewal windows and checkpoint boundaries overlap in
  // strict mode. This is a conservative safety margin.
  uint64_t margin = config.slotsPerEpoch;

  const uint64_t requiredBlocks = checkpoint_.currentId + config.checkpoint.minBlocks + margin;

  if (getNextBlockId() < requiredBlocks) {
    return false;
  }
  if (getBlockAgeSeconds(checkpoint_.currentId) <
      config.checkpoint.minAgeSeconds) {
    return false;
  }
  return true;
}

Chain::Checkpoint Chain::getCheckpoint() const {
  return checkpoint_;
}

uint64_t Chain::getNextBlockId() const { return ledger_.getNextBlockId(); }

int64_t Chain::getConsensusTimestamp() const {
  return consensus_.getTimestamp();
}

int64_t Chain::getSlotStartTime(uint64_t slot) const {
  return consensus_.getSlotStartTime(slot);
}

uint64_t Chain::getSlotDuration() const { return optChainConfig_.has_value() ? optChainConfig_.value().slotDuration : 0; }

uint64_t Chain::getCurrentSlot() const { return consensus_.getCurrentSlot(); }

uint64_t Chain::getCurrentEpoch() const { return consensus_.getCurrentEpoch(); }

uint64_t Chain::getTotalStake() const { return consensus_.getTotalStake(); }

uint64_t Chain::getStakeholderStake(uint64_t stakeholderId) const {
  return consensus_.getStake(stakeholderId);
}

uint64_t Chain::getMaxTransactionsPerBlock() const {
  return optChainConfig_.has_value() ? optChainConfig_.value().maxTransactionsPerBlock : 0;
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

Chain::Roe<Ledger::ChainNode> Chain::readBlock(uint64_t blockId) const {
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

Chain::Roe<Client::UserAccount>
Chain::getUserAccountMetaFromBlock(const Ledger::Block &block,
                                   uint64_t accountId) const {
  auto matchesUserAccount = [&](const Ledger::Transaction &tx) -> bool {
    switch (tx.type) {
    case Ledger::Transaction::T_NEW_USER:
      return accountId != AccountBuffer::ID_GENESIS &&
             tx.toWalletId == accountId;
    case Ledger::Transaction::T_USER:
      return accountId != AccountBuffer::ID_GENESIS &&
             tx.fromWalletId == accountId && tx.toWalletId == accountId;
    case Ledger::Transaction::T_RENEWAL:
      return tx.fromWalletId == accountId &&
             accountId != AccountBuffer::ID_GENESIS;
    default:
      return false;
    }
  };

  for (auto it = block.signedTxes.rbegin(); it != block.signedTxes.rend();
       ++it) {
    const auto &tx = it->obj;
    if (!matchesUserAccount(tx)) {
      continue;
    }
    Client::UserAccount userAccount;
    if (!userAccount.ltsFromString(tx.meta)) {
      return Error(E_INTERNAL_DESERIALIZE,
                   "Failed to deserialize account info: " +
                       std::to_string(tx.meta.size()) + " bytes");
    }
    return userAccount;
  }

  return Error(E_INTERNAL, "No prior user/renewal from this account in block");
}

Chain::Roe<Chain::GenesisAccountMeta>
Chain::getGenesisAccountMetaFromBlock(const Ledger::Block &block) const {
  auto matchesAccount = [&](const Ledger::Transaction &tx) -> bool {
    switch (tx.type) {
    case Ledger::Transaction::T_GENESIS:
      // GENESIS record only happens at first block.
      return tx.fromWalletId == AccountBuffer::ID_GENESIS && block.index == 0;
    case Ledger::Transaction::T_CONFIG:
    case Ledger::Transaction::T_RENEWAL:
      return tx.fromWalletId == AccountBuffer::ID_GENESIS;
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

    GenesisAccountMeta gm;
    if (!gm.ltsFromString(tx.meta)) {
      return Error(E_INTERNAL_DESERIALIZE,
                   "Failed to deserialize checkpoint: " +
                       std::to_string(tx.meta.size()) + " bytes");
    }
    return gm;
  }

  return Error(E_INTERNAL,
               "No prior checkpoint/user/renewal from this account in block");
}

Chain::Roe<std::string> Chain::getUpdatedAccountMetadataForRenewal(
    const Ledger::Block &block, const AccountBuffer::Account &account,
    uint64_t minFee) const {
  if (account.id == AccountBuffer::ID_GENESIS) {
    auto metaResult = getGenesisAccountMetaFromBlock(block);
    if (!metaResult) {
      return metaResult.error();
    }
    auto gm = metaResult.value();
    gm.genesis.wallet = account.wallet;

    auto it = gm.genesis.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
    if (it != gm.genesis.wallet.mBalances.end()) {
      const int64_t fee = static_cast<int64_t>(minFee);
      if (it->second < std::numeric_limits<int64_t>::min() + fee) {
        return Error(E_TX_VALIDATION,
                     "Genesis balance underflow while applying renewal fee");
      }
      it->second -= fee;
    }

    return gm.ltsToString();
  }

  auto userAccountRoe = getUserAccountMetaFromBlock(block, account.id);
  if (!userAccountRoe) {
    return userAccountRoe.error();
  }
  Client::UserAccount userAccount = std::move(userAccountRoe.value());
  userAccount.wallet = account.wallet;

  // verifyBalance expects "expected" = post-renewal balance (current - fee)
  auto it = userAccount.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
  if (it != userAccount.wallet.mBalances.end()) {
    const int64_t fee = static_cast<int64_t>(minFee);
    if (account.id == AccountBuffer::ID_FEE) {
      // Fee account renewing itself: fee is paid to self, so balance may go
      // negative temporarily; check for underflow only.
      if (it->second < std::numeric_limits<int64_t>::min() + fee) {
        return Error(E_TX_VALIDATION,
                     "Fee account balance underflow while applying renewal fee");
      }
    } else {
      if (it->second < fee) {
        return Error(E_ACCOUNT_BALANCE,
                     "Insufficient genesis balance for renewal fee");
      }
      it->second -= fee;
    }
  }
  return userAccount.ltsToString();
}

Chain::Roe<Ledger::SignedData<Ledger::Transaction>>
Chain::createRenewalTransaction(uint64_t accountId) const {
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
  tx.idempotentId = 0;
  tx.validationTsMin = 0;
  tx.validationTsMax = 0;

  // Compute minimum fee from current account metadata state.
  auto minimumFeeResult = calculateMinimumFeeForAccountMeta(bank_, accountId);
  if (!minimumFeeResult) {
    return minimumFeeResult.error();
  }
  const uint64_t minimumFee = minimumFeeResult.value();

  if (accountId != AccountBuffer::ID_GENESIS &&
      accountId != AccountBuffer::ID_FEE) {
    auto balance = bank_.getBalance(accountId, AccountBuffer::ID_GENESIS);
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
    auto blockResult = ledger_.readBlock(account.blockId);
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
  if (!optChainConfig_.has_value()) {
    if (checkpoint_.currentId > checkpoint_.lastId) {
      return Error(E_INTERNAL,
                   "Chain config not initialized; expected config when "
                   "checkpoint.currentId > checkpoint.lastId");
    }
    return 0;  // No renewals while still syncing
  }
  const BlockChainConfig &config = optChainConfig_.value();
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

  for (uint64_t accountId :
       bank_.getAccountIdsBeforeBlockId(maxBlockIdForRenewal)) {
    auto renewalResult = createRenewalTransaction(accountId);
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

Chain::Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>>
Chain::findTransactionsByWalletId(uint64_t walletId,
                                  uint64_t &ioBlockId) const {
  // ioBlockId is the block ID to start scanning from (exclusive). It is updated
  // to the last scanned block ID. Client sends 0 to mean "latest" (scan from
  // tip); we substitute getNextBlockId() so that scanning runs.

  std::vector<Ledger::SignedData<Ledger::Transaction>> out;
  uint64_t nextId = ledger_.getNextBlockId();
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
    auto blockRoe = ledger_.readBlock(currentBlockId);
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
  const uint64_t firstBlockId = ledger_.getStartingBlockId();
  const uint64_t nextBlockId = ledger_.getNextBlockId();
  if (nextBlockId <= firstBlockId) {
    return Error(E_LEDGER_READ, "No blocks in ledger");
  }

  auto lastBlockRoe = ledger_.readLastBlock();
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
    auto blockRoe = ledger_.readBlock(mid);
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
  if (consensus_.isStakeUpdateNeeded()) {
    auto stakeholders = bank_.getStakeholders();
    consensus_.setStakeholders(stakeholders);
  }
}

void Chain::refreshStakeholders(uint64_t blockSlot) {
  uint64_t epoch = consensus_.getEpochFromSlot(blockSlot);
  if (consensus_.isStakeUpdateNeeded(epoch)) {
    auto stakeholders = bank_.getStakeholders();
    consensus_.setStakeholders(stakeholders, epoch);
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

  // Process blocks from ledger one by one (replay existing chain state)
  // Starting block id is always a checkpoint id
  checkpoint_.lastId = startingBlockId;
  checkpoint_.currentId = startingBlockId;
  uint64_t blockId = startingBlockId;
  uint64_t logInterval = 1000; // Log every 1000 blocks
  // Strict validatation if we are loading from the beginning
  bool isStrictMode = startingBlockId == 0;
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
  auto feeWalletFeeResult =
      calculateMinimumFeeForTransaction(gm.config, feeTx.obj);
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
  auto reserveFeeResult =
      calculateMinimumFeeForTransaction(gm.config, minerTx.obj);
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
  auto recycleFeeResult =
      calculateMinimumFeeForTransaction(gm.config, recycleTx.obj);
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

Chain::Roe<void>
Chain::validateNormalBlock(const Ledger::ChainNode &block, bool isStrictMode) const {
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
    if (!consensus_.validateSlotLeader(slotLeader, slot)) {
      return Error(E_CONSENSUS_SLOT_LEADER,
                   "Invalid slot leader for block at slot " +
                       std::to_string(slot));
    }
    if (!consensus_.validateBlockTiming(block.block.timestamp, slot)) {
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
    if (!optChainConfig_.has_value()) {
      return Error(E_INTERNAL,
                   "Chain config not initialized; expected config in strict mode");
    }
    const uint64_t maxTx = optChainConfig_.value().maxTransactionsPerBlock;
    if (maxTx > 0 && block.block.signedTxes.size() > maxTx) {
      for (const auto &signedTx : block.block.signedTxes) {
        if (signedTx.obj.type != Ledger::Transaction::T_RENEWAL) {
          return Error(E_BLOCK_VALIDATION,
                       "Block has more than max transactions per block (" +
                           std::to_string(block.block.signedTxes.size()) + " > " +
                           std::to_string(maxTx) +
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
        return Error(
            E_TX_IDEMPOTENCY,
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

  if (optChainConfig_.has_value() && needsCheckpoint(optChainConfig_.value()) &&
      block.block.index > checkpoint_.currentId) {
    checkpoint_.lastId = checkpoint_.currentId;
    checkpoint_.currentId = block.block.index;
    log().info << "Checkpoint rotated: last=" << checkpoint_.lastId
               << ", current=" << checkpoint_.currentId;
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
                 "Failed to validate buffer transaction: " + roe.error().message);
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
    return processBufferTransaction(bank, tx);
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
  case Ledger::Transaction::T_GENESIS:
    return processSystemInit(tx);
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
    return processTransaction(tx, blockId, isStrictMode);
  }
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
            std::to_string(int(account.wallet.minSignatures)) +
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
      if (crypto_.verify(account.wallet.keyType, publicKey, message,
                         signature)) {
        keyUsed[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      log().error << "Invalid signature for account " +
                         std::to_string(account.id) + ": " +
                         utl::toJsonSafeString(signature);
      log().error << "Expected signatures: "
                  << int(account.wallet.minSignatures);
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
                   "Failed to get account when validating transaction signatures: " + accountResult.error().message);
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
  if (idempotentId == 0) {
    return {};
  }
  const int64_t tsStart = consensus_.getSlotStartTime(slotMin);
  auto startBlockRoe = ledger_.findBlockByTimestamp(tsStart);
  if (!startBlockRoe) {
    // No blocks at or after window start -> no duplicate
    return {};
  }
  const uint64_t nextBlockId = ledger_.getNextBlockId();
  for (uint64_t blockId = startBlockRoe.value().block.index;
       blockId < nextBlockId; ++blockId) {
    auto blockRoe = ledger_.readBlock(blockId);
    if (!blockRoe) {
      break;
    }
    const auto &block = blockRoe.value().block;
    if (block.slot > slotMax) {
      break;
    }
    if (block.slot < slotMin) {
      continue;
    }
    for (const auto &signedTx : block.signedTxes) {
      if (signedTx.obj.idempotentId == idempotentId &&
          signedTx.obj.fromWalletId == fromWalletId) {
        return Error(
            E_TX_IDEMPOTENCY,
            "Duplicate idempotent id: " + std::to_string(idempotentId) +
                " for wallet: " + std::to_string(fromWalletId));
      }
    }
  }
  return {};
}

Chain::Roe<void> Chain::validateIdempotencyRules(const Ledger::Transaction &tx,
                                                 uint64_t effectiveSlot, bool isStrictMode) const {
  if (!isStrictMode) {
    return {};
  }
  if (tx.idempotentId == 0) {
    // Special value to skip idempotency check, this is dangerous, use it with caution
    return {};
  }
  if (tx.validationTsMax < tx.validationTsMin) {
    return Error(
        E_TX_VALIDATION,
        "Validation window invalid: validationTsMax < validationTsMin");
  }
  if (!optChainConfig_.has_value()) {
    return Error(E_TX_VALIDATION,
                 "Chain config not initialized; expected config in strict mode");
  }
  const uint64_t spanSeconds =
      static_cast<uint64_t>(tx.validationTsMax - tx.validationTsMin);
  const auto &config = optChainConfig_.value();

  if (spanSeconds > config.maxValidationTimespanSeconds) {
    return Error(E_TX_VALIDATION_TIMESPAN,
                 "Validation window exceeds max timespan: " +
                     std::to_string(spanSeconds) + " > " +
                     std::to_string(config.maxValidationTimespanSeconds));
  }
  const uint64_t slotMin = consensus_.getSlotFromTimestamp(tx.validationTsMin);
  const uint64_t slotMax = consensus_.getSlotFromTimestamp(tx.validationTsMax);
  if (effectiveSlot < slotMin || effectiveSlot > slotMax) {
    return Error(E_TX_TIME_OUTSIDE_WINDOW,
                 "Current time (slot " + std::to_string(effectiveSlot) +
                     ") outside validation window [slot " +
                     std::to_string(slotMin) + ", " + std::to_string(slotMax) +
                     "]");
  }
  // Only enforce idempotency against transactions that occurred strictly
  // before the current effective slot (submit time or block slot), so that
  // we do not treat the transaction being validated (or other txs in the same
  // block) as duplicates.
  if (effectiveSlot == 0 || effectiveSlot <= slotMin) {
    // No earlier slots within the validation window to check.
    return {};
  }
  const uint64_t slotMaxIdempotency = effectiveSlot - 1;
  return checkIdempotency(tx.idempotentId, tx.fromWalletId, slotMin,
                          slotMaxIdempotency);
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
  optChainConfig_ = gm.config;

  // Reset consensus parameters
  auto config = consensus_.getConfig();

  if (config.genesisTime == 0) {
    config.genesisTime = optChainConfig_.value().genesisTime;
  } else if (optChainConfig_.value().genesisTime != config.genesisTime) {
    return Error(E_TX_VALIDATION, "Genesis time mismatch");
  }
  config.slotDuration = optChainConfig_.value().slotDuration;
  config.slotsPerEpoch = optChainConfig_.value().slotsPerEpoch;
  consensus_.init(config);

  AccountBuffer::Account genesisAccount;
  genesisAccount.id = AccountBuffer::ID_GENESIS;
  genesisAccount.blockId = 0;
  genesisAccount.wallet = gm.genesis.wallet;
  auto roeAddGenesis = bank_.add(genesisAccount);
  if (!roeAddGenesis) {
    return Error(E_INTERNAL_BUFFER,
                 "Failed to add genesis account to buffer: " +
                     roeAddGenesis.error().message);
  }

  log().info << "System initialized";
  log().info << "  Version: " << gm.VERSION;
  log().info << "  Config: " << optChainConfig_.value();
  log().info << "  Genesis: " << gm.genesis;

  return {};
}

Chain::Roe<Chain::GenesisAccountMeta>
Chain::processSystemUpdateImpl(AccountBuffer &bank,
                               const Ledger::Transaction &tx,
                               uint64_t blockId, bool isStrictMode) const {
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

  if (gm.genesis.wallet.publicKeys.size() < 3) {
    return Error(E_TX_VALIDATION,
                 "Genesis account must have at least 3 public keys");
  }

  if (gm.genesis.wallet.minSignatures < 2) {
    return Error(E_TX_VALIDATION,
                 "Genesis account must have at least 2 signatures");
  }

  if (isStrictMode) {
    if (gm.config.genesisTime != optChainConfig_.value().genesisTime) {
      return Error(E_TX_VALIDATION, "Genesis time mismatch");
    }

    if (gm.config.slotDuration > optChainConfig_.value().slotDuration) {
      return Error(E_TX_VALIDATION, "Slot duration cannot be increased");
    }

    if (gm.config.slotsPerEpoch < optChainConfig_.value().slotsPerEpoch) {
      return Error(E_TX_VALIDATION, "Slots per epoch cannot be decreased");
    }
  }

  if (!bank.verifyBalance(AccountBuffer::ID_GENESIS, 0, 0,
                          gm.genesis.wallet.mBalances)) {
    return Error(E_TX_VALIDATION, "Genesis account balance mismatch");
  }

  bank.remove(AccountBuffer::ID_GENESIS);

  AccountBuffer::Account account;
  account.id = AccountBuffer::ID_GENESIS;
  account.blockId = blockId;
  account.wallet = gm.genesis.wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return Error(E_INTERNAL_BUFFER, "Failed to add updated genesis account: " +
                                        addResult.error().message);
  }

  return gm;
}

Chain::Roe<void> Chain::processSystemUpdate(const Ledger::Transaction &tx,
                                            uint64_t blockId,
                                            bool isStrictMode) {
  log().info << "Processing system update transaction";
  auto result = processSystemUpdateImpl(bank_, tx, blockId, isStrictMode);
  if (!result) {
    return result.error();
  }
  optChainConfig_ = result.value().config;
  log().info << "System updated";
  log().info << "  Version: " << GenesisAccountMeta::VERSION;
  log().info << "  Config: " << optChainConfig_.value();
  return {};
}

Chain::Roe<void> Chain::processGenesisRenewalImpl(AccountBuffer &bank,
                                                  const Ledger::Transaction &tx,
                                                  uint64_t blockId,
                                                  bool isBufferMode,
                                                  bool isStrictMode) const {
  if (tx.fromWalletId != AccountBuffer::ID_GENESIS ||
      tx.toWalletId != AccountBuffer::ID_GENESIS) {
    return Error(
        E_TX_VALIDATION,
        "Genesis renewal must use genesis wallet (ID_GENESIS -> ID_GENESIS)");
  }
  if (tx.tokenId != AccountBuffer::ID_GENESIS) {
    return Error(E_TX_VALIDATION,
                 "Genesis renewal must use genesis token (ID_GENESIS)");
  }
  if (tx.amount != 0) {
    return Error(E_TX_VALIDATION,
                 "Genesis renewal transaction must have amount 0");
  }

  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.meta)) {
    return Error(E_INTERNAL_DESERIALIZE,
                 "Failed to deserialize genesis renewal meta: " +
                     std::to_string(tx.meta.size()) + " bytes");
  }

  if (gm.genesis.wallet.publicKeys.size() < 3) {
    return Error(E_TX_VALIDATION,
                 "Genesis account must have at least 3 public keys");
  }
  if (gm.genesis.wallet.minSignatures < 2) {
    return Error(E_TX_VALIDATION,
                 "Genesis account must have at least 2 signatures");
  }

  if (isStrictMode) {
    auto minimumFeeResult = calculateMinimumFeeForTransaction(optChainConfig_.value(), tx);
    if (!minimumFeeResult) {
      return minimumFeeResult.error();
    }
    const uint64_t minFeePerTransaction = minimumFeeResult.value();
    if (tx.fee < minFeePerTransaction) {
      return Error(E_TX_FEE, "Genesis renewal fee below minimum: " +
                                 std::to_string(tx.fee));
    }
  }

  auto genesisAccountResult = bank.getAccount(AccountBuffer::ID_GENESIS);
  if (!genesisAccountResult) {
    if (isStrictMode) {
      return Error(E_ACCOUNT_NOT_FOUND,
                   "Genesis account not found for renewal");
    }
    return {};
  }

  if (isBufferMode && tx.fee > 0) {
    auto feeRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_FEE);
    if (!feeRoe) {
      return feeRoe;
    }
  }

  if (!bank.verifyBalance(AccountBuffer::ID_GENESIS, 0, tx.fee,
                          gm.genesis.wallet.mBalances)) {
    return Error(E_TX_VALIDATION,
                 "Genesis account balance mismatch in renewal");
  }

  bank.remove(AccountBuffer::ID_GENESIS);

  AccountBuffer::Account account;
  account.id = AccountBuffer::ID_GENESIS;
  account.blockId = blockId;
  account.wallet = gm.genesis.wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return Error(E_INTERNAL_BUFFER, "Failed to add renewed genesis account: " +
                                        addResult.error().message);
  }

  if (tx.fee > 0 && bank.hasAccount(AccountBuffer::ID_FEE)) {
    auto depositResult = bank.depositBalance(
        AccountBuffer::ID_FEE, AccountBuffer::ID_GENESIS,
        static_cast<int64_t>(tx.fee));
    if (!depositResult) {
      return Error(E_TX_TRANSFER,
                   "Failed to credit fee to fee account: " +
                       depositResult.error().message);
    }
  }

  return {};
}

Chain::Roe<void> Chain::processGenesisRenewal(const Ledger::Transaction &tx,
                                              uint64_t blockId,
                                              bool isStrictMode) {
  log().info << "Processing genesis renewal transaction";
  auto result = processGenesisRenewalImpl(bank_, tx, blockId, false, isStrictMode);
  if (!result) {
    return result.error();
  }
  log().info << "Genesis account renewed";
  return {};
}

Chain::Roe<void> Chain::processUserInitImpl(AccountBuffer &bank,
                                            const Ledger::Transaction &tx,
                                            uint64_t blockId,
                                            bool isBufferMode, bool isStrictMode) const {
  if (isStrictMode) {
    auto minimumFeeResult = calculateMinimumFeeForTransaction(optChainConfig_.value(), tx);
    if (!minimumFeeResult) {
      return minimumFeeResult.error();
    }
    const uint64_t minFeePerTransaction = minimumFeeResult.value();
    if (tx.fee < minFeePerTransaction) {
      return Error(E_TX_FEE, "New user transaction fee below minimum: " +
                                 std::to_string(tx.fee));
    }
  }

  bool toWalletExists = bank.hasAccount(tx.toWalletId) ||
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

  auto spendingResult = bank.verifySpendingPower(
      tx.fromWalletId, AccountBuffer::ID_GENESIS, tx.amount, tx.fee);
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

  if (!crypto_.isSupported(userAccount.wallet.keyType)) {
    return Error(E_TX_VALIDATION,
                 "Unsupported key type: " +
                     std::to_string(int(userAccount.wallet.keyType)));
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

  auto transferResult =
      bank.transferBalance(tx.fromWalletId, tx.toWalletId,
                           AccountBuffer::ID_GENESIS, tx.amount, tx.fee);
  if (!transferResult) {
    return Error(E_TX_TRANSFER, "Failed to transfer balance: " +
                                    transferResult.error().message);
  }

  return {};
}

Chain::Roe<void> Chain::processUserInit(const Ledger::Transaction &tx,
                                        uint64_t blockId, bool isStrictMode) {
  log().info << "Processing user initialization transaction";
  auto result = processUserInitImpl(bank_, tx, blockId, false, isStrictMode);
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
  auto accountResult = bank.getAccount(accountId);
  if (!accountResult) {
    return Error(E_ACCOUNT_NOT_FOUND,
                 "User account not found: " + std::to_string(accountId));
  }

  auto blockResult = ledger_.readBlock(accountResult.value().blockId);
  if (!blockResult) {
    return Error(E_BLOCK_NOT_FOUND,
                 "Block not found: " +
                     std::to_string(accountResult.value().blockId));
  }

  size_t metaSize = 0;

  if (accountId == AccountBuffer::ID_GENESIS) {
    auto metaResult = getGenesisAccountMetaFromBlock(blockResult.value().block);
    if (!metaResult) {
      return metaResult.error();
    }

    metaSize = metaResult.value().genesis.meta.size();
  } else {
    auto userMetaResult =
        getUserAccountMetaFromBlock(blockResult.value().block, accountId);
    if (!userMetaResult) {
      return userMetaResult.error();
    }
    metaSize = userMetaResult.value().meta.size();
  }

  if (metaSize > optChainConfig_.value().maxCustomMetaSize) {
    return Error(E_TX_VALIDATION,
                 "Custom metadata exceeds maxCustomMetaSize: " +
                     std::to_string(metaSize) + " > " +
                     std::to_string(optChainConfig_.value().maxCustomMetaSize));
  }

  const uint64_t nonFreeMetaSize =
      metaSize > optChainConfig_.value().freeCustomMetaSize
          ? static_cast<uint64_t>(metaSize) - optChainConfig_.value().freeCustomMetaSize
          : 0ULL;

  return calculateMinimumFeeFromNonFreeMetaSize(optChainConfig_.value(), nonFreeMetaSize);
}

Chain::Roe<void> Chain::processUserAccountUpsertImpl(
    AccountBuffer &bank, const Ledger::Transaction &tx, uint64_t blockId,
    bool isBufferMode, bool isStrictMode) const {
  if (tx.tokenId != AccountBuffer::ID_GENESIS) {
    return Error(E_TX_VALIDATION,
                 "User update transaction must use genesis token (ID_GENESIS)");
  }

  if (tx.fromWalletId != tx.toWalletId) {
    return Error(
        E_TX_VALIDATION,
        "User update transaction must use same from and to wallet IDs");
  }

  if (isStrictMode) {
    auto minimumFeeResult = calculateMinimumFeeForTransaction(optChainConfig_.value(), tx);
    if (!minimumFeeResult) {
      return minimumFeeResult.error();
    }
    const uint64_t minFeePerTransaction = minimumFeeResult.value();
    if (tx.fee < minFeePerTransaction) {
      return Error(E_TX_FEE, "User update transaction fee below minimum: " +
                                 std::to_string(tx.fee));
    }
  }

  if (tx.amount != 0) {
    return Error(E_TX_VALIDATION, "User update transaction must have amount 0");
  }

  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(tx.meta)) {
    return Error(E_INTERNAL_DESERIALIZE,
                 "Failed to deserialize user meta for account " +
                     std::to_string(tx.fromWalletId) + ": " +
                     std::to_string(tx.meta.size()) + " bytes");
  }

  if (!crypto_.isSupported(userAccount.wallet.keyType)) {
    return Error(E_TX_VALIDATION,
                 "Unsupported key type: " +
                     std::to_string(int(userAccount.wallet.keyType)));
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
    if (tx.fee > 0 && tx.fromWalletId != AccountBuffer::ID_FEE) {
      auto feeRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_FEE);
      if (!feeRoe) {
        return feeRoe;
      }
    }
  }

  auto bufferAccountResult = bank.getAccount(tx.fromWalletId);
  if (!bufferAccountResult) {
    if (isStrictMode) {
      return Error(E_ACCOUNT_NOT_FOUND, "User account not found in buffer: " +
                                            std::to_string(tx.fromWalletId));
    }
  } else {
    auto balanceVerifyResult = bank.verifyBalance(tx.fromWalletId, 0, tx.fee,
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

  if (tx.fee > 0 && bank.hasAccount(AccountBuffer::ID_FEE)) {
    auto depositResult = bank.depositBalance(
        AccountBuffer::ID_FEE, AccountBuffer::ID_GENESIS,
        static_cast<int64_t>(tx.fee));
    if (!depositResult) {
      return Error(E_TX_TRANSFER,
                   "Failed to credit fee to fee account: " +
                       depositResult.error().message);
    }
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

  auto minimumFeeResult =
      calculateMinimumFeeForAccountMeta(bank, tx.fromWalletId);
  if (!minimumFeeResult) {
    return minimumFeeResult.error();
  }
  const uint64_t minFeePerTransaction = minimumFeeResult.value();
  if (bank.getBalance(tx.fromWalletId, AccountBuffer::ID_GENESIS) >=
      static_cast<int64_t>(minFeePerTransaction)) {
    return Error(E_TX_VALIDATION, "User account must have less than " +
                                      std::to_string(minFeePerTransaction) +
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
                                       uint64_t blockId, bool isStrictMode) {
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
  if (tx.fee > 0) {
    auto feeRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_FEE);
    if (!feeRoe) {
      return feeRoe;
    }
  }
  return strictProcessTransaction(bank, tx);
}

Chain::Roe<void> Chain::processBufferUserInit(AccountBuffer &bank,
                                              const Ledger::Transaction &tx,
                                              uint64_t blockId) const {
  return processUserInitImpl(bank, tx, blockId, true, true);
}

Chain::Roe<void> Chain::processBufferSystemUpdate(AccountBuffer &bank,
                                                  const Ledger::Transaction &tx,
                                                  uint64_t blockId) const {
  auto genesisRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_GENESIS);
  if (!genesisRoe) {
    return genesisRoe;
  }
  auto configResult = processSystemUpdateImpl(bank, tx, blockId, true);
  if (!configResult) {
    return configResult.error();
  }
  return {};
}

Chain::Roe<void>
Chain::processBufferUserAccountUpsert(AccountBuffer &bank,
                                      const Ledger::Transaction &tx,
                                      uint64_t blockId) const {
  return processUserAccountUpsertImpl(bank, tx, blockId, true, true);
}

Chain::Roe<void>
Chain::processBufferGenesisRenewal(AccountBuffer &bank,
                                   const Ledger::Transaction &tx,
                                   uint64_t blockId) const {
  auto genesisRoe = ensureAccountInBuffer(bank, AccountBuffer::ID_GENESIS);
  if (!genesisRoe) {
    return genesisRoe;
  }
  return processGenesisRenewalImpl(bank, tx, blockId, true, true);
}

Chain::Roe<void>
Chain::processBufferUserEnd(AccountBuffer &bank,
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
  auto minimumFeeResult = calculateMinimumFeeForTransaction(optChainConfig_.value(), tx);
  if (!minimumFeeResult) {
    return minimumFeeResult.error();
  }
  const uint64_t minFeePerTransaction = minimumFeeResult.value();
  if (tx.fee < minFeePerTransaction) {
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
          tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount, tx.fee);
      if (!transferResult) {
        return Error(E_TX_TRANSFER, "Failed to transfer balance: " +
                                        transferResult.error().message);
      }
    } else {
      // To unknown wallet: deduct amount and fee from sender, credit fee to
      // ID_FEE
      if (tx.tokenId == AccountBuffer::ID_GENESIS) {
        auto withdrawResult = bank_.withdrawBalance(
            tx.fromWalletId, tx.tokenId,
            static_cast<int64_t>(tx.amount) + static_cast<int64_t>(tx.fee));
        if (!withdrawResult) {
          return Error(E_TX_TRANSFER, "Failed to withdraw balance: " +
                                          withdrawResult.error().message);
        }
      } else {
        auto withdrawAmountResult = bank_.withdrawBalance(
            tx.fromWalletId, tx.tokenId, static_cast<int64_t>(tx.amount));
        if (!withdrawAmountResult) {
          return Error(E_TX_TRANSFER, "Failed to withdraw balance: " +
                                          withdrawAmountResult.error().message);
        }
        if (tx.fee > 0) {
          auto withdrawFeeResult =
              bank_.withdrawBalance(tx.fromWalletId, AccountBuffer::ID_GENESIS,
                                    static_cast<int64_t>(tx.fee));
          if (!withdrawFeeResult) {
            return Error(E_TX_TRANSFER, "Failed to withdraw fee: " +
                                            withdrawFeeResult.error().message);
          }
        }
      }
      if (tx.fee > 0 && bank_.hasAccount(AccountBuffer::ID_FEE)) {
        auto depositFeeResult = bank_.depositBalance(
            AccountBuffer::ID_FEE, AccountBuffer::ID_GENESIS,
            static_cast<int64_t>(tx.fee));
        if (!depositFeeResult) {
          return Error(E_TX_TRANSFER, "Failed to credit fee: " +
                                          depositFeeResult.error().message);
        }
      }
    }
  } else {
    // From unknown wallet
    if (bank_.hasAccount(tx.toWalletId)) {
      auto depositResult = bank_.depositBalance(
          tx.toWalletId, tx.tokenId, static_cast<int64_t>(tx.amount));
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
