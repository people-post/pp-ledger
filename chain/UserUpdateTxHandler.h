#ifndef PP_LEDGER_USER_UPDATE_TX_HANDLER_H
#define PP_LEDGER_USER_UPDATE_TX_HANDLER_H

#include "UserAccountUpsertBase.h"

namespace pp {

class UserUpdateTxHandler final : public UserAccountUpsertBase {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;
};

} // namespace pp

#endif
