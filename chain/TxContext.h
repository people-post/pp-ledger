#ifndef PP_LEDGER_TX_CONTEXT_H
#define PP_LEDGER_TX_CONTEXT_H

#include "Types.h"
#include "AccountBuffer.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "lib/common/Crypto.h"

#include <optional>

namespace pp {

struct ChainTxContext {
  Ledger &ledger;
  AccountBuffer &bank;
  std::optional<BlockChainConfig> &optChainConfig;
  consensus::Ouroboros &consensus;
  Crypto &crypto;
  Checkpoint &checkpoint;
};

/** Read-only subsystem view; safe to build from `const Chain` (e.g. const buffer paths). */
struct ChainTxContextConst {
  const Ledger &ledger;
  const AccountBuffer &bank;
  const std::optional<BlockChainConfig> &optChainConfig;
  const consensus::Ouroboros &consensus;
  const Crypto &crypto;
  const Checkpoint &checkpoint;
};

} // namespace pp

#endif
