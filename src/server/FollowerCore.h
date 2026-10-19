#ifndef PP_LEDGER_FOLLOWER_CORE_H
#define PP_LEDGER_FOLLOWER_CORE_H

#include "server/NodeCore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pp {

/**
 * A node that follows an upstream (Relay, Miner): it opens its ledger at a starting block, adds
 * the blocks it syncs, checks the beacon's signed tips, and notes the certificates and equivocation
 * evidence upstreams and peers send it.
 */
class FollowerCore : public NodeCore {
public:
  /** Add a synced block, then offer what waited on it (the stores). */
  virtual Roe<void> addBlock(const Ledger::ChainNode &block);

  /** A beacon signed tip, of the generation in force here or after `pending` (Chain::verifySignedTip). */
  Roe<void> verifySignedTip(const Client::SignedTip &tip, const std::vector<const Ledger::Block *> &pending = {}) const;
  /** The beacon authority at the local tip (Chain::authorityTrail). */
  Roe<AuthorityTrail> authorityTrail() const;
  /** Whether an upstream serving `tip` is worth following (Chain::mayFollowTip). */
  bool mayFollowTip(const Client::SignedTip &tip) const { return chain_.mayFollowTip(tip); }
  /** The block the held certificate vouches for (Chain::vouchedCutover). */
  std::optional<std::pair<uint64_t, std::string>> vouchedCutover() const { return chain_.vouchedCutover(); }
  /** Recalibrated to the beacon's clock (Chain::setTimeOffsetMs). */
  void setTimeOffsetMs(int64_t offsetMs) { chain_.setTimeOffsetMs(offsetMs); }

  /**
   * A beacon certificate from outside the blocks (ba5c: STATUS, TX_ADD), kept and persisted if it
   * is the highest seen (CertificateStore::note). None when refused (logged).
   */
  std::optional<Chain::CertificateOffer> noteCertificate(const Ledger::Record &record, const std::string &from);
  /**
   * Equivocation evidence (ba6: found in tips, STATUS, EQUIVOCATION_ADD), kept and persisted when it
   * verifies (EquivocationStore::note): the node then commits nothing of that generation above its
   * height. None when refused (logged).
   */
  std::optional<Chain::EquivocationOffer> noteEquivocation(const Client::Equivocation &evidence,
                                                           const std::string &from);

protected:
  using NodeCore::NodeCore;

  /**
   * Open the ledger under `workDir`: apply a pending rewind, mount the ledger (one ending below
   * `startingBlockId` is too old: it is removed) or create it at `startingBlockId`, load the chain
   * on the beacon's clock (`timeOffsetMs`), and open the stores.
   */
  Roe<void> openLedger(const std::string &workDir, int64_t timeOffsetMs, uint64_t startingBlockId);
};

} // namespace pp

#endif // PP_LEDGER_FOLLOWER_CORE_H
