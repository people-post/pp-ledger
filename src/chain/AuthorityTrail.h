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
 * that point signed it. The chain of signatures starts at the node's own state, so blocks from an
 * untrusted upstream cannot move the trail to a key the admin set did not certify. verifyTip()
 * then checks a signed tip against the generation in force at the trail's end: the tip's own
 * generation, its key; an older generation is superseded.
 */
class AuthorityTrail {
public:
  AuthorityTrail(std::string networkId, ChainAuthority authority, Client::Wallet admin);

  /** Follow `block` (the next one after the last observed). */
  chain_tx::Roe<void> observe(const Ledger::Block &block);

  /** A tip signed by the generation in force at the trail's end, for this network. */
  chain_tx::Roe<void> verifyTip(const Client::SignedTip &tip) const;

  const BeaconCertificate &beacon() const { return authority_.beacon; }

  /**
   * The authority a starting block carries: genesis (its T_GENESIS) or a checkpoint block (its
   * genesis renewal). None for other blocks.
   */
  static std::optional<ChainAuthority> carriedBy(const Ledger::Block &block);

private:
  chain_tx::Roe<void> verifyAdminSigned(const Ledger::Record &record) const;
  chain_tx::Roe<void> observeConfig(const Ledger::Record &record);
  chain_tx::Roe<void> observeCertificate(const Ledger::Record &record, uint64_t blockId);

  std::string networkId_;
  ChainAuthority authority_;
  Client::Wallet admin_;
  Crypto crypto_;
  mutable logging::Logger logger_;
};

} // namespace pp

#endif // PP_LEDGER_AUTHORITY_TRAIL_H
