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

  /** Follow `block` (the next one after the last observed). */
  chain_tx::Roe<void> observe(const Ledger::Block &block);
  /** Follow one record of block `blockId`; records that do not change the authority are ignored. */
  chain_tx::Roe<void> observeRecord(const Ledger::Record &record, uint64_t blockId);
  /** True for the records that change the authority (what an AuthorityProof carries). */
  static bool changesAuthority(const Ledger::Record &record);

  /**
   * Replay `proof` from `genesis` up to block `upToBlockId` (beacon-authority ba5b): steps in chain
   * order, each signed by the admin set in force at its point. Returns the authority in force at
   * that block — which a checkpoint there must carry.
   */
  static chain_tx::Roe<ChainAuthority> replayProof(const Ledger::Block &genesis, const Client::AuthorityProof &proof,
                                                   uint64_t upToBlockId);

  /** A tip signed by the generation in force at the trail's end, for this network. */
  chain_tx::Roe<void> verifyTip(const Client::SignedTip &tip) const;

  const BeaconCertificate &beacon() const { return authority_.beacon; }
  const ChainAuthority &authority() const { return authority_; }
  const Client::Wallet &admin() const { return admin_; }

  /**
   * The authority a starting block carries: genesis (its T_GENESIS) or a checkpoint block (its
   * genesis renewal). None for other blocks.
   */
  static std::optional<ChainAuthority> carriedBy(const Ledger::Block &block);

private:
  chain_tx::Roe<void> verifyAdminSigned(const Ledger::Record &record) const;
  chain_tx::Roe<void> observeConfig(const Ledger::Record &record);
  chain_tx::Roe<void> observeGenesisRenewal(const Ledger::Record &record);
  chain_tx::Roe<void> observeCertificate(const Ledger::Record &record, uint64_t blockId);

  std::string networkId_;
  ChainAuthority authority_;
  Client::Wallet admin_;
  Crypto crypto_;
  mutable logging::Logger logger_;
};

} // namespace pp

#endif // PP_LEDGER_AUTHORITY_TRAIL_H
