#ifndef PP_LEDGER_USER_ACCOUNT_UPSERT_BASE_H
#define PP_LEDGER_USER_ACCOUNT_UPSERT_BASE_H

#include "ledger/chain/ITxHandler.h"

namespace pp {

/**
 * Shared implementation for user-account upsert semantics (T_USER_UPDATE and
 * renewal-to-user-upsert mapping).
 *
 * Provides common buffer/block entrypoints and the core applyUserAccountUpsert()
 * implementation that mutates the working AccountBuffer.
 */
class UserAccountUpsertBase : public ITxHandler {
protected:
  /** Who wrote the upsert. */
  enum class UpsertKind {
    /** T_USER_UPDATE, signed by the account: may change its keys and meta; non-zero idempotent id. */
    Update,
    /**
     * T_RENEWAL, signed by the slot leader (idempotent id 0): re-records the account as it is —
     * its keys, threshold, key type and meta unchanged; only the fee leaves its balance.
     */
    Renewal,
    /** A renewal of a system account (genesis renews through its own path): also fee-free. */
    SystemRenewal,
  };

  chain_tx::Roe<void>
  applyUserUpdateBufferCommon(const Ledger::TxUserUpdate &tx,
                              AccountBuffer &bank,
                              const BufferApplyContext &c,
                              UpsertKind kind = UpsertKind::Update) const;

  chain_tx::Roe<void>
  applyUserUpdateBlockCommon(const Ledger::TxUserUpdate &tx,
                             AccountBuffer &bank,
                             const BlockApplyContext &c,
                             UpsertKind kind = UpsertKind::Update) const;

  chain_tx::Roe<void>
  applyUserAccountUpsert(const Ledger::TxUserUpdate &tx,
                         const TxContext &ctx, AccountBuffer &bank,
                         uint64_t blockId, bool isBufferMode,
                         chain_block::BlockAdmissionMode admissionMode,
                         UpsertKind kind) const;

private:
  /** A renewal re-records the committed account: same keys, threshold, key type and meta. */
  chain_tx::Roe<void> checkRenewalKeepsAccount(const Client::UserAccount &carried,
                                               const AccountBuffer::Account &held) const;
};
} // namespace pp

#endif

