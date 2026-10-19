#ifndef PP_LEDGER_BLOCK_ADD_POLICY_H
#define PP_LEDGER_BLOCK_ADD_POLICY_H

#include "../ledger/Ledger.h"

namespace pp {

/** How a BLOCK_ADD relates to what this node already holds. */
enum class BlockAddCheck {
  New,         ///< Not held yet: validate and add (a gateway forwards it up).
  AlreadyHave, ///< Same block already held: succeed without doing the work again.
  Conflicts,   ///< A different block holds that id: refuse.
};

/**
 * Checked before validating a BLOCK_ADD, so a repeat (a leader retrying after
 * a lost reply, or a resend) is answered cheaply and is not forwarded again.
 * `chain` provides getNextBlockId / readBlock / calculateHash (Beacon, Relay).
 */
template <typename ChainLike> BlockAddCheck checkBlockAdd(const ChainLike &chain, const Ledger::ChainNode &block) {
  if (block.block.index >= chain.getNextBlockId()) {
    return BlockAddCheck::New;
  }
  auto stored = chain.readBlock(block.block.index);
  if (!stored) {
    return BlockAddCheck::New; // let addBlock report why it cannot be added
  }
  return stored.value().hash == chain.calculateHash(block.block) ? BlockAddCheck::AlreadyHave
                                                                 : BlockAddCheck::Conflicts;
}

} // namespace pp

#endif // PP_LEDGER_BLOCK_ADD_POLICY_H
