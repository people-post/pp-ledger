#include "BlockChain.h"
#include "Block.h"

#include <algorithm>
#include <openssl/evp.h>
#include <stdexcept>

namespace pp {

// BlockChain implementation
BlockChain::BlockChain() {
  setLogger("BlockChain");
  // No auto-genesis block - blocks must be added explicitly
}

// Blockchain operations
bool BlockChain::addBlock(std::shared_ptr<Block> block) {
  if (!block) {
    return false;
  }

  chain_.push_back(block);
  return true;
}

std::shared_ptr<iii::Block> BlockChain::getLatestBlock() const {
  if (chain_.empty()) {
    return nullptr;
  }
  return chain_.back();
}

std::shared_ptr<Block> BlockChain::getLatestConcreteBlock() const {
  if (chain_.empty()) {
    return nullptr;
  }
  return chain_.back();
}

std::shared_ptr<Block> BlockChain::getBlock(uint64_t index) const {
  if (index >= chain_.size()) {
    return nullptr;
  }
  return chain_[index];
}

size_t BlockChain::getSize() const { return chain_.size(); }

bool BlockChain::isValid() const {
  if (chain_.empty()) {
    return false;
  }

  // Validate all blocks in the chain
  for (size_t i = 0; i < chain_.size(); i++) {
    const auto &currentBlock = chain_[i];

    // Verify current block's hash
    if (currentBlock->getHash() != currentBlock->calculateHash()) {
      return false;
    }

    // Verify link to previous block (skip for first block if it has special
    // previousHash "0")
    // TODO: Skip validation by index saved in block, not by position in chain
    if (i > 0) {
      const auto &previousBlock = chain_[i - 1];
      if (currentBlock->getPreviousHash() != previousBlock->getHash()) {
        return false;
      }
    }
  }

  return true;
}

bool BlockChain::validateBlock(const Block &block) const {
  // Verify block's hash
  if (block.getHash() != block.calculateHash()) {
    return false;
  }

  return true;
}

std::vector<std::shared_ptr<Block>>
BlockChain::getBlocks(uint64_t fromIndex, uint64_t toIndex) const {
  std::vector<std::shared_ptr<Block>> result;

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

std::string BlockChain::getLastBlockHash() const {
  if (chain_.empty()) {
    return "0";
  }
  return chain_.back()->getHash();
}

size_t BlockChain::trimBlocks(size_t count) {
  if (count == 0 || chain_.empty()) {
    return 0; // Nothing to trim or empty chain
  }

  // Trim from the head (beginning) of the chain
  size_t toRemove = std::min(count, chain_.size());
  chain_.erase(chain_.begin(), chain_.begin() + toRemove);

  return toRemove;
}

} // namespace pp
