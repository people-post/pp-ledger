#ifndef PP_LEDGER_RENEWAL_TX_HANDLER_H
#define PP_LEDGER_RENEWAL_TX_HANDLER_H

#include "UserAccountUpsertBase.h"

namespace pp {

/** T_RENEWAL when renewing the genesis account (miner-signed). */
class RenewalTxHandler final : public UserAccountUpsertBase {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

private:
  chain_tx::Roe<void>
  applyRenewal(const Ledger::TxRenewal &tx, const TxContext &ctx,
               AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
               bool isStrictMode) const;
};

} // namespace pp

#endif
