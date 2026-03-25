#ifndef PP_LEDGER_CHAIN_TRANSACTION_CONTEXT_H
#define PP_LEDGER_CHAIN_TRANSACTION_CONTEXT_H

#include "ChainTypes.h"
#include "AccountBuffer.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "lib/common/Crypto.h"
#include "lib/common/Logger.h"

#include <optional>

namespace pp {

struct ChainTransactionContext {
  Ledger &ledger;
  AccountBuffer &bank;
  std::optional<BlockChainConfig> &optChainConfig;
  consensus::Ouroboros &consensus;
  Crypto &crypto;
  Checkpoint &checkpoint;
  logging::Logger &logger;
};

} // namespace pp

#endif
