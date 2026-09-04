#include "UserUpdateTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxTyped.h"
#include "Types.h"

#include <variant>

namespace pp {

chain_tx::Roe<size_t>
UserUpdateTxHandler::getBillableCustomMetaSizeForFee(
    const BlockChainConfig &config, const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxUserUpdate>(
      tx, "getBillableCustomMetaSizeForFee", "TxUserUpdate");
  if (!pRoe) {
    return pRoe.error();
  }
  return chain_tx::billableUserCustomMetaSize(config, pRoe.value()->meta);
}

chain_tx::Roe<uint64_t>
UserUpdateTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                        uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  auto pRoe = chain_tx::expectTx<Ledger::TxUserUpdate>(tx, "getSignerAccountId",
                                                       "TxUserUpdate");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->walletId;
}

chain_tx::Roe<bool>
UserUpdateTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                           uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxUserUpdate>(
      tx, "matchesWalletForIndex", "TxUserUpdate");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->walletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
UserUpdateTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxUserUpdate>(tx, "getIdempotencyKey",
                                                       "TxUserUpdate");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(
      std::make_pair(p->walletId, p->idempotentId));
}

chain_tx::Roe<void> UserUpdateTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                    AccountBuffer &bank,
                                                    const BlockApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxUserUpdate>(tx, "applyBlock", "TxUserUpdate");
  if (!pRoe) {
    return pRoe.error();
  }
  return applyUserUpdateBlockCommon(*pRoe.value(), bank, c);
}

chain_tx::Roe<void> UserUpdateTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                     AccountBuffer &bank,
                                                     const BufferApplyContext &c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxUserUpdate>(tx, "applyBuffer",
                                                       "TxUserUpdate");
  if (!pRoe) {
    return pRoe.error();
  }
  return applyUserUpdateBufferCommon(*pRoe.value(), bank, c);
}

std::optional<std::string>
UserUpdateTxHandler::getUserAccountMetaForTx(const Ledger::TypedTx &tx,
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
