#ifndef PP_LEDGER_ACCOUNT_POLICY_H
#define PP_LEDGER_ACCOUNT_POLICY_H

#include "AccountBuffer.h"
#include "TxError.h"
#include "Types.h"
#include "../client/Client.h"
#include "lib/common/Crypto.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace pp::chain_tx {

using FnPublisherExists = std::function<bool(uint64_t)>;

/** Key type / publicKeys / minSignatures checks shared by user-account paths. */
Roe<void> validateUserWalletBasics(const Crypto &crypto,
                                   const Client::Wallet &wallet);

/**
 * Parse + canonicalize account attachment. `errorPrefix` is prepended to the
 * parse error (e.g. "Account attachment: " / "Genesis account attachment: ").
 */
Roe<std::string>
validateAndCanonicalizeAttachment(const std::string &raw,
                                  const FnPublisherExists &publisherExists,
                                  std::string_view errorPrefix);

/** Genesis wallet must have ≥3 keys and ≥2 required signatures. */
Roe<void> validateGenesisWalletShape(const Client::Wallet &wallet);

/** Remove + re-add genesis account with the given wallet at `blockId`. */
Roe<void> replaceGenesisAccount(AccountBuffer &bank, uint64_t blockId,
                                const Client::Wallet &wallet);

/**
 * Billable (pre-free-tier) inner custom-meta size for a serialized
 * Client::UserAccount blob. Returns 0 when the outer tx meta is within the
 * free tier.
 */
Roe<size_t> billableUserCustomMetaSize(const BlockChainConfig &config,
                                       const std::string &serializedUserAccount);

} // namespace pp::chain_tx

#endif
