#pragma once

#include "../interface/BlockChain.hpp"
#include "Block.h"
#include "Module.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pp {

/**
 * Concrete implementation of BlockChain data structure
 *
 * Manages an in-memory chain of blocks.
 * Storage management is handled by Ledger.
 */
class BlockChain : public Module, public iii::BlockChain {
public:
  BlockChain();
  ~BlockChain() = default;

  // Blockchain operations (also implements iii::BlockChain interface)
  std::shared_ptr<iii::Block> getLatestBlock() const override;
  size_t getSize() const override;

  // Convenience method to get concrete Block type
  std::shared_ptr<Block> getLatestConcreteBlock() const;

  // Additional blockchain operations
  bool addBlock(std::shared_ptr<Block> block);
  std::shared_ptr<Block> getBlock(uint64_t index) const;
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
  bool validateBlock(const Block &block) const;
  std::vector<std::shared_ptr<Block>> getBlocks(uint64_t fromIndex,
                                                uint64_t toIndex) const;

  std::vector<std::shared_ptr<Block>> chain_;
};

} // namespace pp
