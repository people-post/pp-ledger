#ifndef PP_LEDGER_CHAIN_TX_SIGNATURES_H
#define PP_LEDGER_CHAIN_TX_SIGNATURES_H

#include "ChainTxError.h"
#include "AccountBuffer.h"
#include "lib/common/Crypto.h"
#include "lib/common/Logger.h"
#include "../ledger/Ledger.h"

#include <string>
#include <vector>

namespace pp::chain_tx {

Roe<void> verifySignaturesAgainstAccount(
    const Ledger::Transaction &tx, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account, const Crypto &crypto,
    logging::Logger &logger);

} // namespace pp::chain_tx

#endif
