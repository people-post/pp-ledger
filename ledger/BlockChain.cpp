#include "../server/Validator.h"
#include "../ledger/Ledger.h"

#include <algorithm>
#include <openssl/evp.h>
#include <stdexcept>

namespace pp {

// BlockChain implementation
Validator::BlockChain::BlockChain() {
  // No auto-genesis block - blocks must be added explicitly
}

// Blockchain operations
bool Validator::BlockChain::addBlock(std::shared_ptr<Ledger::Block> block) {
  if (!block) {
    return false;
  }

  chain_.push_back(block);
  return true;
}

std::shared_ptr<Ledger::Block> Validator::BlockChain::getLatestBlock() const {
  if (chain_.empty()) {
    return nullptr;
  }
  return chain_.back();
}

std::shared_ptr<Ledger::Block> Validator::BlockChain::getBlock(uint64_t index) const {
  if (index >= chain_.size()) {
    return nullptr;
  }
  return chain_[index];
}

size_t Validator::BlockChain::getSize() const { return chain_.size(); }

bool Validator::BlockChain::isValid() const {
  if (chain_.empty()) {
    return false;
  }

  // Validate all blocks in the chain
  for (size_t i = 0; i < chain_.size(); i++) {
    const auto &currentBlock = chain_[i];

    // Verify current block's hash
    if (currentBlock->hash != currentBlock->calculateHash()) {
      return false;
    }

    // Verify link to previous block (skip for first block if it has special
    // previousHash "0")
    // TODO: Skip validation by index saved in block, not by position in chain
    if (i > 0) {
      const auto &previousBlock = chain_[i - 1];
      if (currentBlock->previousHash != previousBlock->hash) {
        return false;
      }
    }
  }

  return true;
}

bool Validator::BlockChain::validateBlock(const Ledger::Block &block) const {
  // Verify block's hash
  if (block.hash != block.calculateHash()) {
    return false;
  }

  return true;
}

std::vector<std::shared_ptr<Ledger::Block>>
Validator::BlockChain::getBlocks(uint64_t fromIndex, uint64_t toIndex) const {
  std::vector<std::shared_ptr<Ledger::Block>> result;

  if (fromIndex > toIndex || fromIndex >= chain_.size()) {
    return result;
  }

  uint64_t endIndex =
      std::min(toIndex + 1, static_cast<uint64_t>(chain_.size()));
  for (uint64_t i = fromIndex; i < endIndex; i++) {
    result.push_back(chain_[i]);
  }

  return result;
}

std::string Validator::BlockChain::getLastBlockHash() const {
  if (chain_.empty()) {
    return "0";
  }
  return chain_.back()->hash;
}

size_t Validator::BlockChain::trimBlocks(size_t count) {
  if (count == 0 || chain_.empty()) {
    return 0; // Nothing to trim or empty chain
  }

  // Trim from the head (beginning) of the chain
  size_t toRemove = std::min(count, chain_.size());
  chain_.erase(chain_.begin(), chain_.begin() + toRemove);

  return toRemove;
}

} // namespace pp
