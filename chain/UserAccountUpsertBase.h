#ifndef PP_LEDGER_USER_ACCOUNT_UPSERT_BASE_H
#define PP_LEDGER_USER_ACCOUNT_UPSERT_BASE_H

#include "ITxHandler.h"

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
  chain_tx::Roe<void>
  applyUserUpdateBufferCommon(const Ledger::TxUserUpdate &tx,
                              AccountBuffer &bank,
                              const BufferApplyContext &c);

  chain_tx::Roe<void>
  applyUserUpdateBlockCommon(const Ledger::TxUserUpdate &tx,
                             AccountBuffer &bank,
                             const BlockApplyContext &c);

  chain_tx::Roe<void>
  applyUserAccountUpsert(const Ledger::TxUserUpdate &tx,
                         const TxContext &ctx, AccountBuffer &bank,
                         uint64_t blockId, bool isBufferMode,
                         bool isStrictMode);
};

} // namespace pp

#endif

