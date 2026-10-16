#ifndef PP_LEDGER_AUTHORITY_TRAIL_H
#define PP_LEDGER_AUTHORITY_TRAIL_H

#include "TxError.h"
#include "Types.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "common/Logger.h"
#include "lib/common/Crypto.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pp {

/**
 * The beacon authority along a run of blocks that are not committed yet (beacon-authority ba5),
 * starting from what a node holds: its network, the certificate in force and the admin set
 * (genesis account wallet).
 *
 * observe() follows each block's authority changes — a config update replacing the admin set, a
 * beacon certificate raising the generation — and takes one only if the admin set in force at
 * that point signed it; a genesis renewal must agree with what it followed. The chain of signatures starts at the node's own state, so blocks from an
 * untrusted upstream cannot move the trail to a key the admin set did not certify. verifyTip()
 * then checks a signed tip against the generation in force at the trail's end: the tip's own
 * generation, its key; an older generation is superseded.
 */
class AuthorityTrail {
public:
  AuthorityTrail(std::string networkId, ChainAuthority authority, Client::Wallet admin);

  /** The authority genesis sets: its certificate and recovery set, its admin set. */
  static chain_tx::Roe<AuthorityTrail> fromGenesis(const Ledger::Block &genesis);
  /**
   * The authority a node with no blocks learns from its first one (beacon-authority ba5b):
   * genesis (its T_GENESIS) or a checkpoint (its opening genesis renewal). Call observe() on the
   * same block next, for the rest of its records.
   */
  static chain_tx::Roe<AuthorityTrail> fromStartingBlock(const Ledger::Block &block);

  /** Follow `block` (the next one after the last observed): a recovery due at its epoch first. */
  chain_tx::Roe<void> observe(const Ledger::Block &block);
  /**
   * Follow one record of block `blockId` in `epoch`; records that do not change the authority are
   * ignored. Call enterEpoch(epoch) first.
   */
  chain_tx::Roe<void> observeRecord(const Ledger::Record &record, uint64_t blockId, uint64_t epoch);
  /** A pending recovery whose epoch has come replaces the admin set (ba7), as the chain does. */
  void enterEpoch(uint64_t epoch);
  /** True for the records that change the authority (what an AuthorityProof carries). */
  static bool changesAuthority(const Ledger::Record &record);

  /**
   * Replay `proof` from `genesis` up to block `upToBlockId` (beacon-authority ba5b): steps in chain
   * order, each signed by the admin set in force at its point. Returns the trail there: the
   * authority and admin set a checkpoint at that block must carry.
   */
  static chain_tx::Roe<AuthorityTrail> replayProof(const Ledger::Block &genesis, const Client::AuthorityProof &proof,
                                                   uint64_t upToBlockId, uint64_t upToEpoch);

  /** Whether a starting block's authority and admin set are the ones this trail reached. */
  chain_tx::Roe<void> agrees(const ChainAuthority &authority, const Client::Wallet &admin) const;

  /**
   * A later certificate the node holds out of band (beacon-authority ba5c): the admin set signed
   * it, but no block followed here commits it yet. verifyTip() then refuses a tip of an older
   * generation above its cutover, whatever key signed it.
   */
  void setSeenCertificate(std::optional<BeaconCertificate> seen) { seen_ = std::move(seen); }
  /**
   * Equivocation evidence the node holds (beacon-authority ba6): verifyTip() then refuses a tip of
   * its generation above its height.
   */
  void setEquivocation(std::optional<Client::Equivocation> evidence) { equivocation_ = std::move(evidence); }

  /**
   * A tip signed by the generation in force at the trail's end, for this network, and not above
   * the cutover of a later certificate seen out of band.
   */
  chain_tx::Roe<void> verifyTip(const Client::SignedTip &tip) const;

  const BeaconCertificate &beacon() const { return authority_.beacon; }
  const ChainAuthority &authority() const { return authority_; }
  const Client::Wallet &admin() const { return admin_; }


private:
  chain_tx::Roe<void> verifyAdminSigned(const Ledger::Record &record) const;
  chain_tx::Roe<void> observeConfig(const Ledger::Record &record, uint64_t epoch);
  chain_tx::Roe<void> observeGenesisRenewal(const Ledger::Record &record);
  chain_tx::Roe<void> observeCertificate(const Ledger::Record &record, uint64_t blockId, uint64_t epoch);
  chain_tx::Roe<void> observeRecovery(const Ledger::Record &record, uint64_t blockId, uint64_t epoch);

  std::string networkId_;
  ChainAuthority authority_;
  Client::Wallet admin_;
  std::optional<BeaconCertificate> seen_;
  std::optional<Client::Equivocation> equivocation_;
  Crypto crypto_;
  mutable logging::Logger logger_;
};

} // namespace pp

#endif // PP_LEDGER_AUTHORITY_TRAIL_H
