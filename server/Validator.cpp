#include "Validator.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

std::string Validator::SystemCheckpoint::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Validator::SystemCheckpoint::ltsFromString(const std::string& str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

std::string Validator::AccountInfo::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Validator::AccountInfo::ltsFromString(const std::string& str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

Validator::Validator() {
  redirectLogger("Validator");
  ledger_.redirectLogger(log().getFullName() + ".Ledger");
  consensus_.redirectLogger(log().getFullName() + ".Obo");
}

bool Validator::isValidSlotLeader(const Ledger::ChainNode& block) const {
  return consensus_.isSlotLeader(block.block.slot, block.block.slotLeader);
}

bool Validator::isValidTimestamp(const Ledger::ChainNode& block) const {
  int64_t slotStartTime = consensus_.getSlotStartTime(block.block.slot);
  int64_t slotEndTime = consensus_.getSlotEndTime(block.block.slot);
  
  int64_t blockTime = block.block.timestamp;

  if (blockTime < slotStartTime || blockTime > slotEndTime) {
    log().warning << "Block timestamp out of slot range";
    return false;
  }

  return true;
}

bool Validator::isValidBlockSequence(const Ledger::ChainNode& block) const {
  if (block.block.index != ledger_.getNextBlockId()) {
    log().warning << "Invalid block index: expected " << ledger_.getNextBlockId()
                  << " got " << block.block.index;
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
    log().warning << "Invalid block index: expected " << (latestBlock.block.index + 1)
                  << " got " << block.block.index;
    return false;
  }

  // Check previous hash matches
  if (block.block.previousHash != latestBlock.hash) {
    log().warning << "Invalid previous hash";
    return false;
  }

  return true;
}

bool Validator::isChainValid(const std::vector<Ledger::ChainNode>& chain) const {
  size_t chainSize = chain.size();
  
  if (chainSize == 0) {
    return false;
  }

  // Validate all blocks in the chain
  for (size_t i = 0; i < chainSize; i++) {
    const auto &currentBlock = chain[i];
    // Verify current block's hash
    if (currentBlock.hash != calculateHash(currentBlock.block)) {
      return false;
    }

    // Verify link to previous block (skip for first block if it has special
    // previousHash "0")
    // TODO: Skip validation by index saved in block, not by position in chain
    if (i > 0) {
      const auto &previousBlock = chain[i - 1];
      if (currentBlock.block.previousHash != previousBlock.hash) {
        return false;
      }
    }
  }

  return true;
}

uint64_t Validator::getNextBlockId() const {
  return ledger_.getNextBlockId();
}

uint64_t Validator::getCurrentSlot() const {
  return consensus_.getCurrentSlot();
}

uint64_t Validator::getCurrentEpoch() const {
  return consensus_.getCurrentEpoch();
}

uint64_t Validator::getTotalStake() const {
  return consensus_.getTotalStake();
}

std::vector<consensus::Stakeholder> Validator::getStakeholders() const {
  return consensus_.getStakeholders();
}

Validator::Roe<Ledger::ChainNode> Validator::getBlock(uint64_t blockId) const {
  auto result = ledger_.readBlock(blockId);
  if (!result) {
    return Error(8, "Block not found: " + std::to_string(blockId));
  }
  return result.value();
}

std::string Validator::calculateHash(const Ledger::Block& block) const {
  // Use ltsToString() to get the serialized block representation
  std::string serialized = block.ltsToString();
  return utl::sha256(serialized);
}

Validator::Roe<void> Validator::validateGenesisBlock(const Ledger::ChainNode& block) const {
  // Match Beacon::createGenesisBlock exactly: index 0, previousHash "0", nonce 0, slot 0, slotLeader 0
  if (block.block.index != 0) {
    return Error(8, "Genesis block must have index 0");
  }
  if (block.block.previousHash != "0") {
    return Error(8, "Genesis block must have previousHash \"0\"");
  }
  if (block.block.nonce != 0) {
    return Error(8, "Genesis block must have nonce 0");
  }
  if (block.block.slot != 0) {
    return Error(8, "Genesis block must have slot 0");
  }
  if (block.block.slotLeader != 0) {
    return Error(8, "Genesis block must have slotLeader 0");
  }
  // Exactly two transactions: checkpoint and miner/reserve transactions
  if (block.block.signedTxes.size() != 2) {
    return Error(8, "Genesis block must have exactly two transactions");
  }
  
  // First transaction: checkpoint transaction (ID_GENESIS -> ID_GENESIS, amount 0)
  const auto& checkpointTx = block.block.signedTxes[0];
  if (checkpointTx.obj.type != Ledger::Transaction::T_CHECKPOINT) {
    return Error(8, "First genesis transaction must be checkpoint transaction");
  }
  if (checkpointTx.obj.fromWalletId != AccountBuffer::ID_GENESIS || checkpointTx.obj.toWalletId != AccountBuffer::ID_GENESIS) {
    return Error(8, "Genesis checkpoint transaction must use genesis wallet (ID_GENESIS -> ID_GENESIS)");
  }
  if (checkpointTx.obj.amount != 0) {
    return Error(8, "Genesis checkpoint transaction must have amount 0");
  }
  if (checkpointTx.signatures.size() != 1 || checkpointTx.signatures[0] != "genesis") {
    return Error(8, "Genesis checkpoint transaction must have signature \"genesis\"");
  }
  
  // Second transaction: miner/reserve transaction (ID_GENESIS -> ID_RESERVE, INITIAL_TOKEN_SUPPLY)
  const auto& minerTx = block.block.signedTxes[1];
  if (minerTx.obj.type != Ledger::Transaction::T_DEFAULT) {
    return Error(8, "Second genesis transaction must be default transaction");
  }
  if (minerTx.obj.fromWalletId != AccountBuffer::ID_GENESIS || minerTx.obj.toWalletId != AccountBuffer::ID_RESERVE) {
    return Error(8, "Genesis miner transaction must transfer from genesis to reserve wallet");
  }
  if (minerTx.obj.amount + minerTx.obj.fee != AccountBuffer::INITIAL_TOKEN_SUPPLY) {
    return Error(8, "Genesis miner transaction must have amount + fee: " + std::to_string(AccountBuffer::INITIAL_TOKEN_SUPPLY));
  }
  if (minerTx.signatures.size() != 1 || minerTx.signatures[0] != "genesis") {
    return Error(8, "Genesis miner transaction must have signature \"genesis\"");
  }
  
  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
    return Error(10, "Genesis block hash validation failed");
  }
  return {};
}

Validator::Roe<void> Validator::validateBlock(const Ledger::ChainNode& block) const {
  if (block.block.index == 0) {
    return validateGenesisBlock(block);
  }

  // Non-genesis: validate slot leader and timing
  uint64_t slot = block.block.slot;
  uint64_t slotLeader = block.block.slotLeader;
  if (!consensus_.validateSlotLeader(slotLeader, slot)) {
    return Error(6, "Invalid slot leader for block at slot " + std::to_string(slot));
  }
  if (!consensus_.validateBlockTiming(block.block.timestamp, slot)) {
    return Error(7, "Block timestamp outside valid slot range");
  }

  // Validate hash chain (previous block link and index)
  if (block.block.index > 0) {
    auto latestBlockResult = ledger_.readBlock(block.block.index - 1);
    if (!latestBlockResult) {
      return Error(8, "Latest block not found: " + std::to_string(block.block.index - 1));
    }
    auto latestBlock = latestBlockResult.value();
    if (block.block.previousHash != latestBlock.hash) {
      return Error(8, "Block previous hash does not match chain");
    }
    if (block.block.index != latestBlock.block.index + 1) {
      return Error(9, "Block index mismatch");
    }
  }

  // Validate block hash
  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
    return Error(10, "Block hash validation failed");
  }

  // Validate sequence
  if (!isValidBlockSequence(block)) {
    return Error(11, "Invalid block sequence");
  }

  // Validate slot leader
  if (!isValidSlotLeader(block)) {
    return Error(12, "Invalid slot leader");
  }

  // Validate timestamp
  if (!isValidTimestamp(block)) {
    return Error(13, "Invalid timestamp");
  }

  return {};
}

Validator::Roe<void> Validator::addBufferTransaction(AccountBuffer& bufferBank, const Ledger::Transaction& tx) {
  // Filter: Only process transactions matching the buffer's token ID
  if (tx.tokenId != bufferBank.getTokenId()) {
    return {}; // Skip transactions for other tokens
  }
  
  // All transactions happen in bufferBank; initial balances come from bank_
  if (tx.amount < 0) {
    return Error(19, "Transfer amount must be non-negative");
  }
  if (tx.amount == 0) {
    return {};
  }

  // Ensure fromWalletId exists in bufferBank (seed from bank_ if needed)
  if (!bufferBank.has(tx.fromWalletId)) {
    if (bank_.has(tx.fromWalletId)) {
      auto fromAccount = bank_.get(tx.fromWalletId);
      if (!fromAccount) {
        return Error(20, "Failed to get source account from bank: " + fromAccount.error().message);
      }
      auto addResult = bufferBank.add(fromAccount.value());
      if (!addResult) {
        return Error(21, "Failed to add source account to buffer: " + addResult.error().message);
      }
    } else {
      return Error(20, "Source account not found: " + std::to_string(tx.fromWalletId));
    }
  }

  // Ensure toWalletId exists in bufferBank: seed from bank_ or create if not in bank_
  // Track if we newly created toWalletId (not in bank_) so we can remove it on transfer failure
  bool toWalletIdNewlyCreated = false;
  if (!bufferBank.has(tx.toWalletId)) {
    if (bank_.has(tx.toWalletId)) {
      auto toAccount = bank_.get(tx.toWalletId);
      if (!toAccount) {
        return Error(22, "Failed to get destination account from bank: " + toAccount.error().message);
      }
      auto addResult = bufferBank.add(toAccount.value());
      if (!addResult) {
        return Error(23, "Failed to add destination account to buffer: " + addResult.error().message);
      }
    } else {
      AccountBuffer::Account newAccount;
      newAccount.id = tx.toWalletId;
      newAccount.mBalances[tx.tokenId] = 0;
      newAccount.isNegativeBalanceAllowed = (tx.toWalletId == AccountBuffer::ID_GENESIS);
      newAccount.publicKeys = {};
      auto addResult = bufferBank.add(newAccount);
      if (!addResult) {
        return Error(24, "Failed to create destination account in buffer: " + addResult.error().message);
      }
      toWalletIdNewlyCreated = true;
    }
  }

  auto transferResult = bufferBank.transferBalance(tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount);
  if (!transferResult) {
    if (toWalletIdNewlyCreated) {
      bufferBank.remove(tx.toWalletId);
    }
    return Error(25, "Transfer failed: " + transferResult.error().message);
  }
  return {};
}

Validator::Roe<uint64_t> Validator::loadFromLedger(uint64_t startingBlockId, uint64_t tokenId) {
  log().info << "Loading from ledger starting at block ID " << startingBlockId << " for token ID " << tokenId;

  log().info << "Resetting account buffer for token ID " << tokenId;
  bank_.reset(tokenId);

  // Process blocks from ledger one by one
  uint64_t blockId = startingBlockId;
  uint64_t logInterval = 1000; // Log every 1000 blocks
  bool isStrictMode = startingBlockId == 0; // True if we are loading from the beginning (strict validation)
  while (true) {
    auto blockResult = ledger_.readBlock(blockId);
    if (!blockResult) {
      // No more blocks to read
      break;
    }

    Ledger::ChainNode block = blockResult.value();

    auto validateResult = validateBlock(block);
    if (!validateResult) {
      return Error(17, "Block validation failed for block " + std::to_string(blockId) + ": " + validateResult.error().message);
    }

    // Process the block
    auto processResult = processBlock(block, blockId, isStrictMode);
    if (!processResult) {
      return Error(18, "Failed to process block " + std::to_string(blockId) + ": " + processResult.error().message);
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

Validator::Roe<void> Validator::addBlockBase(const Ledger::ChainNode& block, bool isStrictMode) {
  // Validate the block first
  auto validationResult = validateBlock(block);
  if (!validationResult) {
    return Error(3, "Block validation failed: " + validationResult.error().message);
  }

  auto processResult = processBlock(block, block.block.index, isStrictMode);
  if (!processResult) {
    return Error(4, "Failed to process block: " + processResult.error().message);
  }

  auto ledgerResult = ledger_.addBlock(block);
  if (!ledgerResult) {
    return Error(5, "Failed to persist block: " + ledgerResult.error().message);
  }

  log().info << "Block added: " << block.block.index 
             << " from slot leader: " << block.block.slotLeader;

  return {};
}

Validator::Roe<void> Validator::processBlock(const Ledger::ChainNode& block, uint64_t blockId, bool isStrictMode) {
  // Process checkpoint transactions to restore BlockChainConfig
  for (const auto& signedTx : block.block.signedTxes) {
    auto result = processTransaction(signedTx.obj, isStrictMode);
    if (!result) {
      return Error(18, "Failed to process transaction: " + result.error().message);
    }
  }

  return {};
}

Validator::Roe<void> Validator::processTransaction(const Ledger::Transaction& tx, bool isStrictMode) {
  switch (tx.type) {
    case Ledger::Transaction::T_CHECKPOINT:
      return processSystemCheckpoint(tx);
    case Ledger::Transaction::T_USER:
      return processUserCheckpoint(tx);
    case Ledger::Transaction::T_DEFAULT:
      return isStrictMode ? processTransaction(tx) : looseProcessTransaction(tx);
    default:
      return Error(18, "Unknown transaction type: " + std::to_string(tx.type));
  }
}

Validator::Roe<void> Validator::processSystemCheckpoint(const Ledger::Transaction& tx) {
  log().info << "Processing system checkpoint transaction";

  // TODO: Validate system checkpoint transaction fields
  
  // Deserialize BlockChainConfig from transaction metadata
  SystemCheckpoint checkpoint;
  if (!checkpoint.ltsFromString(tx.meta)) {
    return Error(16, "Failed to deserialize checkpoint config: " + tx.meta);
  }

  // Reset chain configuration
  chainConfig_ = checkpoint.config;
  
  // Reset consensus parameters
  auto config = consensus_.getConfig();

  if (config.genesisTime == 0) {
    config.genesisTime = chainConfig_.genesisTime;
  } else if (chainConfig_.genesisTime != config.genesisTime) {
    return Error(17, "Genesis time mismatch");
  }

  config.slotDuration = chainConfig_.slotDuration;
  config.slotsPerEpoch = chainConfig_.slotsPerEpoch;
  consensus_.init(config);

  AccountBuffer::Account genesisAccount;
  genesisAccount.id = AccountBuffer::ID_GENESIS;
  genesisAccount.mBalances[AccountBuffer::ID_GENESIS] = checkpoint.genesis.balance;
  genesisAccount.isNegativeBalanceAllowed = true;
  genesisAccount.publicKeys = checkpoint.genesis.publicKeys;
  auto roeAddGenesis = bank_.add(genesisAccount);
  if (!roeAddGenesis) {
    return Error(26, "Failed to add genesis account to buffer: " + roeAddGenesis.error().message);
  }

  AccountBuffer::Account feeAccount;
  feeAccount.id = AccountBuffer::ID_FEE;
  feeAccount.mBalances[AccountBuffer::ID_GENESIS] = checkpoint.fee.balance;
  feeAccount.isNegativeBalanceAllowed = false;
  feeAccount.publicKeys = checkpoint.fee.publicKeys;
  auto roeAddFee = bank_.add(feeAccount);
  if (!roeAddFee) {
    return Error(27, "Failed to add fee account to buffer: " + roeAddFee.error().message);
  }

  AccountBuffer::Account reserveAccount;
  reserveAccount.id = AccountBuffer::ID_RESERVE;
  reserveAccount.mBalances[AccountBuffer::ID_GENESIS] = checkpoint.reserve.balance;
  reserveAccount.isNegativeBalanceAllowed = false;
  reserveAccount.publicKeys = checkpoint.reserve.publicKeys;
  auto roeAddReserve = bank_.add(reserveAccount);
  if (!roeAddReserve) {
    return Error(28, "Failed to add reserve account to buffer: " + roeAddReserve.error().message);
  }

  log().info << "Restored SystemCheckpoint";
  log().info << "  Version: " << checkpoint.VERSION;
  log().info << "  Config: " << chainConfig_;
  log().info << "  Genesis: " << checkpoint.genesis;
  log().info << "  Fee: " << checkpoint.fee;
  log().info << "  Reserve: " << checkpoint.reserve;

  return {};
}

Validator::Roe<void> Validator::processUserCheckpoint(const Ledger::Transaction& tx) {
  log().info << "Processing user checkpoint transaction";

  // TODO: Validate user checkpoint transaction fields
  
  // Deserialize AccountInfo from transaction metadata
  AccountInfo accountInfo;
  if (!accountInfo.ltsFromString(tx.meta)) {
    return Error(27, "Failed to deserialize user checkpoint: " + tx.meta);
  }
  
  // Populate bank with user balance using toWalletId from transaction
  AccountBuffer::Account account;
  account.id = tx.toWalletId;
  account.mBalances = accountInfo.mBalances;
  account.isNegativeBalanceAllowed = false; // user accounts cannot have negative balance
  account.publicKeys = accountInfo.publicKeys;
  
  auto addResult = bank_.add(account);
  if (!addResult) {
    return Error(28, "Failed to add user account to buffer: " + addResult.error().message);
  }
  
  log().info << "Restored user checkpoint for wallet " << tx.toWalletId;
  log().info << "  Balances: " << accountInfo.mBalances.size() << " tokens";
  log().info << "  Public Keys: " << accountInfo.publicKeys.size();
  
  return {};
}

Validator::Roe<void> Validator::processTransaction(const Ledger::Transaction& tx) {
  if (tx.tokenId != AccountBuffer::ID_GENESIS && tx.tokenId != bank_.getTokenId()) {
    return {}; // Skip transactions for other tokens
  }

  // tx.fromWalletId and tx.toWalletId correspond to bank_.Account.id
  if (tx.amount < 0) {
    return Error(19, "Transfer amount must be non-negative");
  }
  if (tx.amount == 0) {
    return {};
  }

  if (!bank_.has(tx.fromWalletId)) {
    return Error(20, "Source account not found: " + std::to_string(tx.fromWalletId));
  }

  // Only genesis account can have negative balances (enforced by AccountBuffer and account creation)
  // If toWalletId does not exist, create it in bank_
  if (!bank_.has(tx.toWalletId)) {
    if (tx.tokenId != AccountBuffer::ID_GENESIS) {
      return Error(21, "Destination account not found for non-genesis token: " + std::to_string(tx.toWalletId));
    }

    AccountBuffer::Account newAccount;
    newAccount.id = tx.toWalletId;
    newAccount.mBalances[tx.tokenId] = 0;
    newAccount.isNegativeBalanceAllowed = (tx.toWalletId == AccountBuffer::ID_GENESIS);
    newAccount.publicKeys = {};
    auto addResult = bank_.add(newAccount);
    if (!addResult) {
      return Error(21, "Failed to create destination account: " + addResult.error().message);
    }
  }

  // fromWalletId must have sufficient balance (account 0 has isNegativeBalanceAllowed so can go negative)
  auto transferResult = bank_.transferBalance(tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount);
  if (!transferResult) {
    return Error(22, "Transfer failed: " + transferResult.error().message);
  }
  return {};
}

Validator::Roe<void> Validator::looseProcessTransaction(const Ledger::Transaction& tx) {
  log().info << "Loosely processing transaction";

  if (tx.amount < 0) {
    return Error(23, "Transaction must have amount >= 0");
  }

  if (bank_.has(tx.fromWalletId)) {
    if (bank_.has(tx.toWalletId)) {
      auto transferResult = bank_.transferBalance(tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount);
      if (!transferResult) {
        return Error(24, "Failed to transfer balance: " + transferResult.error().message);
      }
    } else {
      // To unknown wallet
      auto withdrawResult = bank_.withdrawBalance(tx.fromWalletId, tx.tokenId, tx.amount);
      if (!withdrawResult) {
        return Error(25, "Failed to withdraw balance: " + withdrawResult.error().message);
      }
    }
  } else {
    // From unknown wallet
    if (bank_.has(tx.toWalletId)) {
      auto depositResult = bank_.depositBalance(tx.toWalletId, tx.tokenId, tx.amount);
      if (!depositResult) {
        return Error(26, "Failed to deposit balance: " + depositResult.error().message);
      }
    } else {
      // From and to unknown wallets, ignore
    }
  }
  return {};
}

} // namespace pp
