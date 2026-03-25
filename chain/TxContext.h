#ifndef PP_LEDGER_TX_CONTEXT_H
#define PP_LEDGER_TX_CONTEXT_H

#include "Types.h"
#include "AccountBuffer.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "lib/common/Crypto.h"

#include <optional>

namespace pp {

/**
 * Chain subsystem bundle passed to transaction handlers.
 * Chain owns a single TxContext member; handlers take TxContext & or
 * const TxContext & depending on whether they may mutate chain state.
 */
struct TxContext {
  Crypto crypto;
  consensus::Ouroboros consensus;
  Ledger ledger;
  AccountBuffer bank;
  std::optional<BlockChainConfig> optChainConfig{std::nullopt};
  Checkpoint checkpoint{};
};

} // namespace pp

#endif
