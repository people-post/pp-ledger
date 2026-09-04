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

/**
 * Publisher-exists predicate for buffer/block apply: self id, working buffer,
 * and (when non-null) the committed buffer used by buffer-mode applies.
 */
FnPublisherExists makeWorkingSetPublisherExists(
    const AccountBuffer &working, const AccountBuffer *committedOrNull,
    uint64_t selfAccountId);

/**
 * Deserialize user-account meta, canonicalize attachment, validate wallet
 * basics. `deserializeErrorMsg` is the full E_INTERNAL_DESERIALIZE message.
 */
Roe<Client::UserAccount> loadAndValidateUserAccountMeta(
    const std::string &serializedMeta, const Crypto &crypto,
    const AccountBuffer &working, const AccountBuffer *committedOrNull,
    uint64_t selfAccountId, std::string_view deserializeErrorMsg);

/** Genesis wallet must have ≥3 keys and ≥2 required signatures. */
Roe<void> validateGenesisWalletShape(const Client::Wallet &wallet);

/** Remove + re-add an account with the given wallet at `blockId`. */
Roe<void> replaceAccount(AccountBuffer &bank, uint64_t accountId,
                         uint64_t blockId, const Client::Wallet &wallet,
                         std::string_view addFailurePrefix);

/** Remove + re-add genesis account with the given wallet at `blockId`. */
Roe<void> replaceGenesisAccount(AccountBuffer &bank, uint64_t blockId,
                                const Client::Wallet &wallet);

/**
 * Credit `fee` to ID_FEE (no-op when fee==0 or fee account absent).
 * `failurePrefix` is prepended to the deposit error message.
 */
Roe<void> creditFeeToFeeAccount(AccountBuffer &bank, uint64_t fee,
                                std::string_view failurePrefix);

/** Map AccountBuffer::Roe errors into chain_tx::TxError. */
Roe<void> mapBufferError(const AccountBuffer::Roe<void> &result);

/** seedFromCommittedIfMissing with AccountBuffer error mapped to TxError. */
Roe<void> seedCommittedAccount(AccountBuffer &working,
                               const AccountBuffer &committed,
                               uint64_t accountId);

/** Seed ID_FEE from committed when `fee > 0`. */
Roe<void> seedFeeAccountIfNeeded(AccountBuffer &working,
                                 const AccountBuffer &committed, uint64_t fee);

/**
 * Billable (pre-free-tier) inner custom-meta size for a serialized
 * Client::UserAccount blob. Returns 0 when the outer tx meta is within the
 * free tier.
 */
Roe<size_t> billableUserCustomMetaSize(const BlockChainConfig &config,
                                       const std::string &serializedUserAccount);

} // namespace pp::chain_tx

#endif
