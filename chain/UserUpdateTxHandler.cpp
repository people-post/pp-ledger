#include "UserUpdateTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t>
UserUpdateTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                        uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getSignerAccountId: expected TxUserUpdate");
  }
  return p->walletId;
}

chain_tx::Roe<bool>
UserUpdateTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                           uint64_t walletId) const {
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "matchesWalletForIndex: expected TxUserUpdate");
  }
  return p->walletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
UserUpdateTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getIdempotencyKey: expected TxUserUpdate");
  }
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(
      std::make_pair(p->walletId, p->idempotentId));
}

chain_tx::Roe<void> UserUpdateTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                    AccountBuffer &bank,
                                                    const BlockApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: expected TxUserUpdate");
  }
  return applyUserUpdateBlockCommon(*p, bank, c);
}

chain_tx::Roe<void> UserUpdateTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                     AccountBuffer &bank,
                                                     const BufferApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer: expected TxUserUpdate");
  }
  return applyUserUpdateBufferCommon(*p, bank, c);
}

std::optional<std::string>
UserUpdateTxHandler::userAccountMetaForTx(const Ledger::TypedTx &tx,
                                          uint64_t accountId) const {
  const auto *p = std::get_if<Ledger::TxUserUpdate>(&tx);
  if (!p) {
    return std::nullopt;
  }
  if (accountId == AccountBuffer::ID_GENESIS || p->walletId != accountId) {
    return std::nullopt;
  }
  return p->meta;
}

} // namespace pp
