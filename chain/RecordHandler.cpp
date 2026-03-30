#include "RecordHandler.h"

#include "ErrorCodes.h"
#include "ConfigTxHandler.h"
#include "DefaultTxHandler.h"
#include "EndUserTxHandler.h"
#include "GenesisTxHandler.h"
#include "NewUserTxHandler.h"
#include "RenewalTxHandler.h"
#include "UserUpdateTxHandler.h"

namespace pp {

RecordHandler::RecordHandler() {
  auto install = [this](std::size_t type, std::unique_ptr<ITxHandler> handler) {
    handlers_[type] = std::move(handler);
  };

  install(Ledger::T_GENESIS, std::make_unique<GenesisTxHandler>());
  install(Ledger::T_CONFIG, std::make_unique<ConfigTxHandler>());
  install(Ledger::T_NEW_USER, std::make_unique<NewUserTxHandler>());
  install(Ledger::T_USER_UPDATE, std::make_unique<UserUpdateTxHandler>());
  install(Ledger::T_DEFAULT, std::make_unique<DefaultTxHandler>());
  install(Ledger::T_RENEWAL, std::make_unique<RenewalTxHandler>());
  install(Ledger::T_END_USER, std::make_unique<EndUserTxHandler>());
}

ITxHandler *RecordHandler::get(std::size_t type) {
  return type < handlers_.size() ? handlers_[type].get() : nullptr;
}

const ITxHandler *RecordHandler::get(std::size_t type) const {
  return type < handlers_.size() ? handlers_[type].get() : nullptr;
}

bool RecordHandler::matchesWalletForIndex(const Ledger::Record &rec,
                                          uint64_t walletId) const {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return false;
  }
  const ITxHandler *handler = get(rec.type);
  if (!handler) {
    return false;
  }
  auto matchRoe = handler->matchesWalletForIndex(typedRoe.value(), walletId);
  return matchRoe && matchRoe.value();
}

chain_tx::Roe<void>
RecordHandler::applyBuffer(const Ledger::Record &rec, AccountBuffer &bank,
                           const BufferApplyContext &ctx) const {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return chain_tx::TxError(
        chain_err::E_INVALID_ARGUMENT,
        "Invalid packed transaction payload: " + typedRoe.error().message);
  }
  const ITxHandler *handler = get(rec.type);
  if (!handler) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Transaction handler not registered for type " +
            std::to_string(rec.type));
  }
  return handler->applyBuffer(typedRoe.value(), bank, ctx);
}

chain_tx::Roe<void>
RecordHandler::applyBlock(const Ledger::Record &rec, AccountBuffer &bank,
                          const BlockApplyContext &ctx) const {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return chain_tx::TxError(
        chain_err::E_INVALID_ARGUMENT,
        "Invalid packed transaction payload: " + typedRoe.error().message);
  }
  const ITxHandler *handler = get(rec.type);
  if (!handler) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Transaction handler not registered for type " +
            std::to_string(rec.type));
  }
  return handler->applyBlock(typedRoe.value(), bank, ctx);
}

void RecordHandler::redirectLoggers(const std::string &baseName) {
  for (std::size_t i = 0; i < handlers_.size(); ++i) {
    if (!handlers_[i]) {
      continue;
    }
    handlers_[i]->redirectLogger(baseName + ".TxHandler." + std::to_string(i));
  }
}

} // namespace pp

