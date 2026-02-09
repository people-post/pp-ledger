#include "Validator.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

std::ostream& operator<<(std::ostream& os, const Validator::CheckpointConfig& config) {
  os << "CheckpointConfig{minBlocks: " << config.minBlocks << ", minAgeSeconds: " << config.minAgeSeconds << "}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const Validator::SingleTokenAccountInfo& info) {
  os << "SingleTokenAccountInfo{balance: " << info.balance << ", publicKeys: " 
     << info.publicKeys.size() << ", meta: \"" << info.meta.size() << " bytes\"}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const Validator::BlockChainConfig& config) {
  os << "BlockChainConfig{genesisTime: " << config.genesisTime << ", "
     << "slotDuration: " << config.slotDuration << ", "
     << "slotsPerEpoch: " << config.slotsPerEpoch << ", "
     << "maxPendingTransactions: " << config.maxPendingTransactions << ", "
     << "maxTransactionsPerBlock: " << config.maxTransactionsPerBlock << ", "
     << "minFeePerTransaction: " << config.minFeePerTransaction << ", "
     << "checkpoint: " << config.checkpoint << "}";
  return os;
}

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

bool Validator::isStakeholderSlotLeader(uint64_t stakeholderId, uint64_t slot) const {
  return consensus_.isSlotLeader(slot, stakeholderId);
}

bool Validator::isSlotBlockProductionTime(uint64_t slot) const {
  return consensus_.isSlotBlockProductionTime(slot);
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

bool Validator::needsCheckpoint(const CheckpointConfig& checkpointConfig) const {
  if (getNextBlockId() < currentCheckpointId_ + checkpointConfig.minBlocks) {
    return false;
  }
  if (getBlockAgeSeconds(currentCheckpointId_) < checkpointConfig.minAgeSeconds) {
    return false;
  }
  return true;
}

uint64_t Validator::getLastCheckpointId() const {
  return lastCheckpointId_;
}

uint64_t Validator::getCurrentCheckpointId() const {
  return currentCheckpointId_;
}

uint64_t Validator::getNextBlockId() const {
  return ledger_.getNextBlockId();
}

int64_t Validator::getConsensusTimestamp() const {
  return consensus_.getTimestamp();
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

uint64_t Validator::getStakeholderStake(uint64_t stakeholderId) const {
  return consensus_.getStake(stakeholderId);
}

Validator::Roe<uint64_t> Validator::getSlotLeader(uint64_t slot) const {
  auto result = consensus_.getSlotLeader(slot);
  if (!result) {
    return Error(15, "Failed to get slot leader: " + result.error().message);
  }
  return result.value();
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

Validator::Roe<Client::AccountInfo> Validator::getAccount(uint64_t accountId) const {
  auto roeAccount = bank_.getAccount(accountId);
  if (!roeAccount) {
    return Error(8, "Account not found: " + std::to_string(accountId));
  }
  auto const& account = roeAccount.value();
  Client::AccountInfo accountInfo;
  accountInfo.mBalances = account.mBalances;
  accountInfo.publicKeys = account.publicKeys;
  return accountInfo;
}

uint64_t Validator::getBlockAgeSeconds(uint64_t blockId) const {
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

std::vector<Ledger::SignedData<Ledger::Transaction>> Validator::collectRenewals(uint64_t slot) const {
  // TODO: Implement renewal collection.
  // Get accounts from bank_ that their block id is too old (comparing to nextBlockId and measured by BlockChainConfig.checkpoint).
  // For each account, create a renewal transaction (account.id -> ID_FEE, amount 0, fee minFeePerTransaction)
  // and add it to the renewal list.
  return {};
}

Validator::Roe<Ledger::ChainNode> Validator::readLastBlock() const {
  auto result = ledger_.readLastBlock();
  if (!result) {
    return Error(2, "Failed to read last block: " + result.error().message);
  }
  return result.value();
}

std::string Validator::calculateHash(const Ledger::Block& block) const {
  // Use ltsToString() to get the serialized block representation
  std::string serialized = block.ltsToString();
  return utl::sha256(serialized);
}

void Validator::initConsensus(const consensus::Ouroboros::Config& config) {
  consensus_.init(config);
}

Validator::Roe<void> Validator::initLedger(const Ledger::InitConfig& config) {
  auto result = ledger_.init(config);
  if (!result) {
    return Error(2, "Failed to initialize ledger: " + result.error().message);
  }
  return {};
}

Validator::Roe<void> Validator::mountLedger(const std::string& workDir) {
  auto result = ledger_.mount(workDir);
  if (!result) {
    return Error(2, "Failed to mount ledger: " + result.error().message);
  }
  return {};
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
  
  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
    return Error(10, "Genesis block hash validation failed");
  }
  return {};
}

Validator::Roe<void> Validator::validateBlock(const Ledger::ChainNode& block) const {
  if (block.block.index == 0) {
    return validateGenesisBlock(block);
  } else {
    return validateNormalBlock(block);
  }
}

Validator::Roe<void> Validator::validateNormalBlock(const Ledger::ChainNode& block) const {
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
  if (tx.fee < chainConfig_.minFeePerTransaction) {
    return Error(18, "Transaction fee below minimum: " + std::to_string(tx.fee));
  }

  // All transactions happen in bufferBank; initial balances come from bank_
  if (tx.amount < 0) {
    return Error(19, "Transfer amount must be non-negative");
  }
  if (tx.amount == 0) {
    return {};
  }

  // Ensure fromWalletId exists in bufferBank (seed from bank_ if needed)
  if (!bufferBank.hasAccount(tx.fromWalletId)) {
    // Try to get from bank_ if not in bufferBank
    if (bank_.hasAccount(tx.fromWalletId)) {
      auto fromAccount = bank_.getAccount(tx.fromWalletId);
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
  if (!bufferBank.hasAccount(tx.toWalletId)) {
    // Try to get from bank_ if not in bufferBank
    if (bank_.hasAccount(tx.toWalletId)) {
      auto toAccount = bank_.getAccount(tx.toWalletId);
      if (!toAccount) {
        return Error(22, "Failed to get destination account from bank: " + toAccount.error().message);
      }
      auto addResult = bufferBank.add(toAccount.value());
      if (!addResult) {
        return Error(23, "Failed to add destination account to buffer: " + addResult.error().message);
      }
    }
  }

  auto addTransactionResult = bufferBank.addTransaction(tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount, tx.fee);
  if (!addTransactionResult) {
    return Error(25, "Transaction failed: " + addTransactionResult.error().message);
  }
  return {};
}

void Validator::refreshStakeholders() {
  if (consensus_.isStakeUpdateNeeded()) {
    auto stakeholders = bank_.getStakeholders();
    consensus_.setStakeholders(stakeholders);
  }
}

Validator::Roe<uint64_t> Validator::loadFromLedger(uint64_t startingBlockId) {
  log().info << "Loading from ledger starting at block ID " << startingBlockId;

  log().info << "Resetting account buffer";
  bank_.reset();

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

    auto const& block = blockResult.value();
    if (blockId != block.block.index) {
      return Error(18, "Block index mismatch: expected " + std::to_string(blockId) + " got " + std::to_string(block.block.index));
    }

    auto processResult = processBlock(block, isStrictMode);
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

Validator::Roe<void> Validator::doAddBlock(const Ledger::ChainNode& block, bool isStrictMode) {
  auto processResult = processBlock(block, isStrictMode);
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

Validator::Roe<void> Validator::processBlock(const Ledger::ChainNode& block, bool isStrictMode) {
  // Validate the block first
  auto validationResult = validateBlock(block);
  if (!validationResult) {
    return Error(17, "Block validation failed for block " + std::to_string(block.block.index) + ": " + validationResult.error().message);
  }

  // Process checkpoint transactions to restore BlockChainConfig
  for (const auto& signedTx : block.block.signedTxes) {
    auto result = processTxRecord(signedTx, isStrictMode);
    if (!result) {
      return Error(18, "Failed to process transaction: " + result.error().message);
    }
  }

  return {};
}

Validator::Roe<void> Validator::processTxRecord(const Ledger::SignedData<Ledger::Transaction>& signedTx, bool isStrictMode) {
  auto roe = validateTxSignatures(signedTx, isStrictMode);
  if (!roe) {
    return Error(18, "Failed to validate transaction: " + roe.error().message);
  }

  auto const& tx = signedTx.obj;
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

Validator::Roe<void> Validator::validateTxSignatures(const Ledger::SignedData<Ledger::Transaction>& signedTx, bool isStrictMode) {
  if (signedTx.signatures.size() < 1) {
    return Error(8, "Transaction must have at least one signature");
  }

  auto accountResult = bank_.getAccount(signedTx.obj.fromWalletId);
  if (!accountResult) {
    if (isStrictMode) {
      if (bank_.isEmpty() && signedTx.obj.fromWalletId == AccountBuffer::ID_GENESIS) {
        // Genesis account is created by the system checkpoint, this is not very good way of handling
        // Should avoid using this generic handlers for specific case
        return {};
      }
      return Error(8, "Failed to get account: " + accountResult.error().message);
    } else {
      // In loose mode, account may not be created before their transactions
      return {};
    }
  }
  auto const& account = accountResult.value();
  if (signedTx.signatures.size() < account.minSignatures) {
    return Error(8, "Account " + std::to_string(signedTx.obj.fromWalletId) + " must have at least " + std::to_string(account.minSignatures) + " signatures, but has " + std::to_string(signedTx.signatures.size()));
  }
  auto message = utl::binaryPack(signedTx.obj);
  std::vector<bool> keyUsed(account.publicKeys.size(), false);
  for (const auto& signature : signedTx.signatures) {
    bool matched = false;
    for (size_t i = 0; i < account.publicKeys.size(); ++i) {
      if (keyUsed[i]) continue;
      const auto& publicKey = account.publicKeys[i];
      if (utl::ed25519Verify(publicKey, message, signature)) {
        keyUsed[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      log().error << "Invalid signature for account " + std::to_string(signedTx.obj.fromWalletId) + ": " + utl::toJsonSafeString(signature);
      log().error << "Expected signatures: " << account.minSignatures;
      for (size_t i = 0; i < account.publicKeys.size(); ++i) {
        log().error << "Public key " << i << ": " << utl::toJsonSafeString(account.publicKeys[i]);
        log().error << "Key used: " << keyUsed[i];
      }
      for (const auto& signature : signedTx.signatures) {
        log().error << "Signature: " << utl::toJsonSafeString(signature);
      }
      return Error(8, "Invalid or duplicate signature for account " + std::to_string(signedTx.obj.fromWalletId));
    }
  }
  return {};
}

Validator::Roe<void> Validator::validateSystemCheckpoint(const Ledger::Transaction& tx) {
  log().info << "Validating system checkpoint transaction";

  if (tx.fromWalletId != AccountBuffer::ID_GENESIS || tx.toWalletId != AccountBuffer::ID_GENESIS) {
    return Error(8, "System checkpoint transaction must use genesis wallet (ID_GENESIS -> ID_GENESIS)");
  }
  if (tx.amount != 0) {
    return Error(8, "System checkpoint transaction must have amount 0");
  }
  if (tx.fee != 0) {
    return Error(8, "System checkpoint transaction must have fee 0");
  }
  return {};
}

Validator::Roe<void> Validator::processSystemCheckpoint(const Ledger::Transaction& tx) {
  log().info << "Processing system checkpoint transaction";

  auto result = validateSystemCheckpoint(tx);
  if (!result) {
    return Error(16, "Failed to validate system checkpoint transaction: " + result.error().message);
  }
  
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
  genesisAccount.publicKeys = checkpoint.genesis.publicKeys;
  genesisAccount.minSignatures = 2;
  auto roeAddGenesis = bank_.add(genesisAccount);
  if (!roeAddGenesis) {
    return Error(26, "Failed to add genesis account to buffer: " + roeAddGenesis.error().message);
  }

  AccountBuffer::Account feeAccount;
  feeAccount.id = AccountBuffer::ID_FEE;
  feeAccount.minSignatures = 2;
  feeAccount.mBalances[AccountBuffer::ID_GENESIS] = checkpoint.fee.balance;
  feeAccount.publicKeys = checkpoint.fee.publicKeys;
  auto roeAddFee = bank_.add(feeAccount);
  if (!roeAddFee) {
    return Error(27, "Failed to add fee account to buffer: " + roeAddFee.error().message);
  }

  AccountBuffer::Account reserveAccount;
  reserveAccount.id = AccountBuffer::ID_RESERVE;
  reserveAccount.mBalances[AccountBuffer::ID_GENESIS] = checkpoint.reserve.balance;
  reserveAccount.publicKeys = checkpoint.reserve.publicKeys;
  reserveAccount.minSignatures = 2;
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
  Client::AccountInfo accountInfo;
  if (!accountInfo.ltsFromString(tx.meta)) {
    return Error(27, "Failed to deserialize user checkpoint: " + tx.meta);
  }
  
  // Populate bank with user balance using toWalletId from transaction
  AccountBuffer::Account account;
  account.id = tx.toWalletId;
  account.mBalances = accountInfo.mBalances;
  account.publicKeys = accountInfo.publicKeys;
  account.minSignatures = accountInfo.minSignatures;
  auto addResult = bank_.add(account);
  if (!addResult) {
    return Error(28, "Failed to add user account to buffer: " + addResult.error().message);
  }
  
  log().info << "Restored user " << tx.toWalletId << " checkpoint: " << accountInfo;
  
  return {};
}

Validator::Roe<void> Validator::processTransaction(const Ledger::Transaction& tx) {
  if (tx.fee < chainConfig_.minFeePerTransaction) {
    return Error(18, "Transaction fee below minimum: " + std::to_string(tx.fee));
  }

  // Use AccountBuffer's addTransaction which handles account creation and balance checks
  auto result = bank_.addTransaction(tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount, tx.fee);
  if (!result) {
    return Error(19, "Transaction failed: " + result.error().message);
  }

  return {};
}

Validator::Roe<void> Validator::looseProcessTransaction(const Ledger::Transaction& tx) {
  if (tx.amount < 0) {
    return Error(23, "Transaction must have amount >= 0");
  }

  // Existing wallets are created by user checkpoints, they have correct balances.
  if (bank_.hasAccount(tx.fromWalletId)) {
    if (bank_.hasAccount(tx.toWalletId)) {
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
    if (bank_.hasAccount(tx.toWalletId)) {
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
