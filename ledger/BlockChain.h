#pragma once

#include "Ledger.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pp {

/**
 * Concrete implementation of BlockChain data structure
 *
 * Manages an in-memory chain of blocks.
 */
class BlockChain {
public:
  BlockChain();
  ~BlockChain() = default;

  // Blockchain operations
  std::shared_ptr<Ledger::Block> getLatestBlock() const;
  size_t getSize() const;

  // Additional blockchain operations
  bool addBlock(std::shared_ptr<Ledger::Block> block);
  std::shared_ptr<Ledger::Block> getBlock(uint64_t index) const;
  bool isValid() const;
  std::string getLastBlockHash() const;

  /**
   * Trim blocks from the head of the chain
   * Removes the first n blocks from the beginning of the chain
   * @param count Number of blocks to trim from the head
   * @return Number of blocks removed
   */
  size_t trimBlocks(size_t count);

private:
  // Internal helper methods
  bool validateBlock(const Ledger::Block &block) const;
  std::vector<std::shared_ptr<Ledger::Block>> getBlocks(uint64_t fromIndex,
                                                uint64_t toIndex) const;

  std::vector<std::shared_ptr<Ledger::Block>> chain_;
};

} // namespace pp
