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

bool Validator::validateBlock(const Ledger::ChainNode& block) const {
  // Verify block's hash
  if (block.hash != calculateHash(block.block)) {
    return false;
  }

  return true;
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
  // Exactly one checkpoint transaction: T_CHECKPOINT, WID_SYSTEM/WID_SYSTEM, amount 0, signature "genesis"
  if (block.block.signedTxes.size() != 1) {
    return Error(8, "Genesis block must have exactly one transaction");
  }
  const auto& tx = block.block.signedTxes[0];
  if (tx.obj.type != Ledger::Transaction::T_CHECKPOINT) {
    return Error(8, "Genesis block must contain checkpoint transaction");
  }
  if (tx.obj.fromWalletId != WID_SYSTEM || tx.obj.toWalletId != WID_SYSTEM) {
    return Error(8, "Genesis checkpoint transaction must use system wallet");
  }
  if (tx.obj.amount != 0) {
    return Error(8, "Genesis checkpoint transaction must have amount 0");
  }
  if (tx.signature != "genesis") {
    return Error(8, "Genesis checkpoint transaction must have signature \"genesis\"");
  }
  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
    return Error(10, "Genesis block hash validation failed");
  }
  return {};
}

Validator::Roe<void> Validator::validateBlockBase(const Ledger::ChainNode& block) const {
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
      newAccount.balance = 0;
      newAccount.isNegativeBalanceAllowed = (tx.toWalletId == WID_SYSTEM);
      newAccount.publicKey = "";
      auto addResult = bufferBank.add(newAccount);
      if (!addResult) {
        return Error(24, "Failed to create destination account in buffer: " + addResult.error().message);
      }
      toWalletIdNewlyCreated = true;
    }
  }

  auto transferResult = bufferBank.transferBalance(tx.fromWalletId, tx.toWalletId, tx.amount);
  if (!transferResult) {
    if (toWalletIdNewlyCreated) {
      bufferBank.remove(tx.toWalletId);
    }
    return Error(25, "Transfer failed: " + transferResult.error().message);
  }
  return {};
}

Validator::Roe<uint64_t> Validator::loadFromLedger(uint64_t startingBlockId) {
  // Process blocks from ledger one by one
  uint64_t blockId = startingBlockId;
  uint64_t logInterval = 1000; // Log every 1000 blocks
  bool isInitMode = startingBlockId > 0; // True if we are loading from a non-zero starting block
  while (true) {
    auto blockResult = ledger_.readBlock(blockId);
    if (!blockResult) {
      // No more blocks to read
      break;
    }

    Ledger::ChainNode block = blockResult.value();
    
    // Process the block
    auto processResult = processBlock(block, blockId, isInitMode);
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

Validator::Roe<void> Validator::addBlockBase(const Ledger::ChainNode& block, bool isInitMode) {
  // Validate the block first
  auto validationResult = validateBlockBase(block);
  if (!validationResult) {
    return Error(3, "Block validation failed: " + validationResult.error().message);
  }

  auto processResult = processBlock(block, block.block.index, isInitMode);
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

Validator::Roe<void> Validator::processBlock(const Ledger::ChainNode& block, uint64_t blockId, bool isInitMode) {
  // Process checkpoint transactions to restore BlockChainConfig
  for (const auto& signedTx : block.block.signedTxes) {
    auto result = processTransaction(signedTx.obj, isInitMode);
    if (!result) {
      return Error(18, "Failed to process transaction: " + result.error().message);
    }
  }

  return {};
}

Validator::Roe<void> Validator::processTransaction(const Ledger::Transaction& tx, bool isInitMode) {
  switch (tx.type) {
    case Ledger::Transaction::T_CHECKPOINT:
      return processSystemCheckpoint(tx);
    case Ledger::Transaction::T_USER:
      return processUserCheckpoint(tx);
    case Ledger::Transaction::T_DEFAULT:
      return isInitMode ? processInitTransaction(tx) : processNormalTransaction(tx);
    default:
      return Error(18, "Unknown transaction type: " + std::to_string(tx.type));
  }
}

Validator::Roe<void> Validator::processSystemCheckpoint(const Ledger::Transaction& tx) {
  log().info << "Processing system checkpoint transaction";
  
  bank_.clear();
  
  // Deserialize BlockChainConfig from transaction metadata
  SystemCheckpoint checkpoint;
  if (!checkpoint.ltsFromString(tx.meta)) {
    return Error(16, "Failed to deserialize checkpoint config: " + tx.meta);
  }
  
  // Restore consensus parameters
  auto config = consensus_.getConfig();

  if (config.genesisTime == 0) {
    config.genesisTime = checkpoint.config.genesisTime;
  } else if (checkpoint.config.genesisTime != config.genesisTime) {
    return Error(17, "Genesis time mismatch");
  }

  config.slotDuration = checkpoint.config.slotDuration;
  config.slotsPerEpoch = checkpoint.config.slotsPerEpoch;
  consensus_.init(config);

  AccountBuffer::Account account;
  account.id = WID_SYSTEM;
  account.balance = checkpoint.genesis.balance;
  account.isNegativeBalanceAllowed = true;
  account.publicKey = checkpoint.genesis.publicKey;
  auto addResult = bank_.add(account);
  if (!addResult) {
    return Error(26, "Failed to add system account to buffer: " + addResult.error().message);
  }

  log().info << "Restored SystemCheckpoint";
  log().info << "  Version: " << checkpoint.VERSION;
  log().info << "  Genesis time: " << checkpoint.config.genesisTime;
  log().info << "  Slot duration: " << checkpoint.config.slotDuration;
  log().info << "  Slots per epoch: " << checkpoint.config.slotsPerEpoch;
  log().info << "  Max pending transactions: " << checkpoint.config.maxPendingTransactions;
  log().info << "  Max transactions per block: " << checkpoint.config.maxTransactionsPerBlock;
  log().info << "  Genesis balance: " << checkpoint.genesis.balance;
  log().info << "  Genesis public key: " << checkpoint.genesis.publicKey;
  log().info << "  Genesis meta: " << checkpoint.genesis.meta;

  return {};
}

Validator::Roe<void> Validator::processUserCheckpoint(const Ledger::Transaction& tx) {
  log().info << "Processing user checkpoint transaction";
  // TODO: Implement user checkpoint processing
  return {};
}

Validator::Roe<void> Validator::processNormalTransaction(const Ledger::Transaction& tx) {
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

  // Only account id 0 can have negative balances (enforced by AccountBuffer and account creation)
  // If toWalletId does not exist, create it in bank_
  if (!bank_.has(tx.toWalletId)) {
    AccountBuffer::Account newAccount;
    newAccount.id = tx.toWalletId;
    newAccount.balance = 0;
    newAccount.isNegativeBalanceAllowed = (tx.toWalletId == WID_SYSTEM);
    newAccount.publicKey = "";
    auto addResult = bank_.add(newAccount);
    if (!addResult) {
      return Error(21, "Failed to create destination account: " + addResult.error().message);
    }
  }

  // fromWalletId must have sufficient balance (account 0 has isNegativeBalanceAllowed so can go negative)
  auto transferResult = bank_.transferBalance(tx.fromWalletId, tx.toWalletId, tx.amount);
  if (!transferResult) {
    return Error(22, "Transfer failed: " + transferResult.error().message);
  }
  return {};
}

Validator::Roe<void> Validator::processInitTransaction(const Ledger::Transaction& tx) {
  log().info << "Processing init transaction";

  if (tx.amount < 0) {
    return Error(23, "Init transaction must have amount >= 0");
  }

  if (bank_.has(tx.fromWalletId)) {
    if (bank_.has(tx.toWalletId)) {
      auto transferResult = bank_.transferBalance(tx.fromWalletId, tx.toWalletId, tx.amount);
      if (!transferResult) {
        return Error(24, "Failed to transfer balance: " + transferResult.error().message);
      }
    } else {
      // To unknown wallet
      auto withdrawResult = bank_.withdrawBalance(tx.fromWalletId, tx.amount);
      if (!withdrawResult) {
        return Error(25, "Failed to withdraw balance: " + withdrawResult.error().message);
      }
    }
  } else {
    // From unknown wallet
    if (bank_.has(tx.toWalletId)) {
      auto depositResult = bank_.depositBalance(tx.toWalletId, tx.amount);
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
