#include "UserUpdateTxHandler.h"
#include "ErrorCodes.h"
#include "../ledger/TypedTx.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t>
UserUpdateTxHandler::getSignerAccountId(const TypedTx &tx,
                                        uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getSignerAccountId: expected TxUserUpdate");
  }
  return p->walletId;
}

chain_tx::Roe<void> UserUpdateTxHandler::applyBlock(const TypedTx &tx,
                                                    AccountBuffer &bank,
                                                    const BlockApplyContext &c) {
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: expected TxUserUpdate");
  }
  return applyUserUpdateBlockCommon(*p, bank, c);
}

chain_tx::Roe<void> UserUpdateTxHandler::applyBuffer(const TypedTx &tx,
                                                     AccountBuffer &bank,
                                                     const BufferApplyContext &c) {
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer: expected TxUserUpdate");
  }
  return applyUserUpdateBufferCommon(*p, bank, c);
}

} // namespace pp
