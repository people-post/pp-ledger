#ifndef PP_LEDGER_ACCOUNT_POLICY_H
#define PP_LEDGER_ACCOUNT_POLICY_H

#include "AccountBuffer.h"
#include "TxError.h"
#include "TxSignatures.h"
#include "Types.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "common/Logger.h"
#include "lib/common/Crypto.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pp::chain_tx {

using FnPublisherExists = std::function<bool(uint64_t)>;

/** Key type / publicKeys / minSignatures checks shared by user-account paths. */
/**
 * A key set a recovery names (the recovery set, or announced next keys): 1 to kMaxSignatureCount
 * distinct ML-DSA-65 keys, 1 to that many to sign.
 */
Roe<void> validateRecoveryKeys(const std::vector<std::string> &keys, uint8_t minSignatures, std::string_view what);

/**
 * An account's recovery as its owner may set it (projects/account-recovery): none (all fields empty),
 * or a valid set and a delay of at least one epoch; never on a system account; never a pending
 * announcement (that is the chain's, set by T_ACCOUNT_RECOVERY).
 */
Roe<void> validateOwnerRecovery(const Client::AccountRecovery &recovery, uint64_t accountId);

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
 * `requireKnownPublishers` false (CheckpointReplay): a node warming up does not hold every account,
 * so the attachment's publisher ids are not checked against its bank.
 */
Roe<Client::UserAccount> loadAndValidateUserAccountMeta(
    const std::string &serializedMeta, const Crypto &crypto,
    const AccountBuffer &working, const AccountBuffer *committedOrNull,
    uint64_t selfAccountId, std::string_view deserializeErrorMsg, bool requireKnownPublishers = true);

/** Genesis wallet must have ≥3 keys and ≥2 required signatures. */
Roe<void> validateGenesisWalletShape(const Client::Wallet &wallet);

/**
 * Chain authority as genesis sets it (beacon-authority ba1): a recovery set of valid, distinct
 * ML-DSA keys none of which is an admin key, 1 ≤ minSignatures ≤ keys, delayEpochs ≥ 1; and a
 * generation-1 beacon certificate for `networkId`, cutover 0, whose key is neither an admin nor a
 * recovery key.
 */
Roe<void> validateGenesisAuthority(const ChainAuthority &authority, const Client::Wallet &adminWallet,
                                   const std::string &networkId);

/**
 * A later beacon certificate against the authority in force (beacon-authority ba2): the chain's
 * `networkId`; a valid key that is new — not the current beacon key, an admin key or a recovery
 * key; `cutoverHeight` not below the current one, and — with a cutover, generation in force above
 * 1 — above it, so the block that committed the generation in force stays (bp2); `cutoverHash`
 * set exactly when there is a cutover; dial hints bounded. On chain (`commitBlockId` set): exactly
 * the next generation, committed no later than the block after its cutover — the previous
 * generation never signs past it (ba5c) — and, with a cutover, exactly in that block (its hash is
 * checked by validateCutoverBlock). Out of band (nullopt): any higher generation.
 */
Roe<void> validateNextBeaconCertificate(const BeaconCertificate &next, const ChainAuthority &current,
                                        const Client::Wallet &adminWallet, const std::string &networkId,
                                        std::optional<uint64_t> commitBlockId);

/** A certificate's dial hints: at most 8, each a non-empty string of at most 256 bytes. */
Roe<void> validateCertificateHints(const BeaconCertificate &cert);

/** On chain, with a cutover: `cutoverBlockHash` (block C's, the one before the commit) is `cutoverHash`. */
Roe<void> validateCutoverBlock(const BeaconCertificate &cert, const std::string &cutoverBlockHash);

/**
 * The certificate in force once block `commitBlockId` commits it (ba5c): its `cutoverHeight` is the
 * last block the previous generation signed, `commitBlockId` − 1 — at or below the signed bound.
 */
BeaconCertificate certificateInForce(BeaconCertificate cert, uint64_t commitBlockId);

/**
 * While a recovery is pending (beacon-authority ba7, B007) the admin set is frozen: it may issue no
 * config update and no beacon generation. `epoch`: the epoch of the block the record would be in
 * (from the apply epoch on, the recovery has replaced the admin set); none out of band (frozen
 * while pending). `what` names the refused record for the error.
 */
Roe<void> requireAdminSetNotFrozen(const ChainAuthority &authority, std::string_view what,
                                   std::optional<uint64_t> epoch = std::nullopt);

/**
 * A recovery announcement against the authority in force (ba7): the chain's `networkId`; no recovery
 * pending already; a new admin set of the genesis shape (≥3 valid distinct keys, 2 ≤ minSignatures ≤
 * keys) none of whose keys is a recovery key or the current beacon key.
 */
Roe<void> validateRecoveryAnnouncement(const RecoveryAnnouncement &announcement, const ChainAuthority &current,
                                       const std::string &networkId);

/** The admin (genesis account) wallet once a recovery applies: its keys and threshold replaced. */
Client::Wallet adminWalletAfterRecovery(Client::Wallet admin, const RecoveryAnnouncement &announcement);

/**
 * The authority and admin wallet as a block of `epoch` holds them (ba7): a pending recovery due by
 * then has replaced the admin set and is no longer pending. A no-op once the chain entered that
 * epoch; what a block drafted ahead of it (renewals) must be built and checked against.
 */
void advanceAuthorityToEpoch(ChainAuthority &authority, Client::Wallet &admin, uint64_t epoch);

/** `record` is signed by enough of the recovery set's keys (over its signing message for `networkId`). */
Roe<void> verifyRecoverySigned(const Ledger::Record &record, const RecoverySet &recovery,
                               const std::string &networkId, const Crypto &crypto, logging::Logger &logger);

/** Remove + re-add an account with the given wallet and attachment (its bytes as recorded) at `blockId`. */
Roe<void> replaceAccount(AccountBuffer &bank, uint64_t accountId,
                         uint64_t blockId, const Client::Wallet &wallet,
                         const std::string &attachment, std::string_view addFailurePrefix);

/** Remove + re-add genesis account with the given wallet and attachment at `blockId`. */
Roe<void> replaceGenesisAccount(AccountBuffer &bank, uint64_t blockId,
                                const Client::Wallet &wallet, const std::string &attachment);

/**
 * Credit `fee` to ID_FEE (no-op when fee==0 or fee account absent).
 * `failurePrefix` is prepended to the deposit error message.
 */
/**
 * A transfer's effects under CheckpointReplay (beacon-authority ba5b): a node warming up from a
 * checkpoint holds only some accounts, and the block was valid, so each leg applies to whichever
 * of payer (amount in `tokenId`, fee in the native token), payee and fee account it holds, exactly
 * as the full chain applied it. A leg on an account not held is skipped; a leg on a held account
 * that fails is an error (the node's state diverged).
 */
Roe<void> applyTransferToHeldAccounts(AccountBuffer &bank, uint64_t fromId, uint64_t toId, uint64_t tokenId,
                                      uint64_t amount, uint64_t fee);

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
