#ifndef PP_LEDGER_TX_SIGNATURES_H
#define PP_LEDGER_TX_SIGNATURES_H

#include "TxError.h"
#include "AccountBuffer.h"
#include "lib/common/Crypto.h"
#include "common/Logger.h"
#include "../ledger/Ledger.h"

#include <string>
#include <vector>

namespace pp::chain_tx {

/**
 * Hard cap on wallet.publicKeys / signatures counts. Signature verification
 * is expensive (ML-DSA-65); without a cap, a wallet with an attacker-chosen
 * number of keys, or a record with an attacker-chosen number of signatures,
 * turns one transaction's verification into an O(N*M) CPU-exhaustion vector.
 * No legitimate multisig setup needs anywhere near this many keys.
 */
constexpr size_t kMaxSignatureCount = 32;

Roe<void> verifySignaturesAgainstAccount(
    const std::string &message, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account, const Crypto &crypto,
    logging::Logger &logger);

} // namespace pp::chain_tx

#endif
