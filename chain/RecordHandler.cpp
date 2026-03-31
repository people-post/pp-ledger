#include "RecordHandler.h"

#include "ErrorCodes.h"
#include "ConfigTxHandler.h"
#include "DefaultTxHandler.h"
#include "EndUserTxHandler.h"
#include "GenesisTxHandler.h"
#include "NewUserTxHandler.h"
#include "RenewalTxHandler.h"
#include "UserUpdateTxHandler.h"

#include <variant>

namespace pp {

namespace {

template <class... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

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

chain_tx::Roe<uint64_t>
RecordHandler::getSignerAccountId(const Ledger::Record &rec,
                                  uint64_t slotLeaderId) const {
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
  auto signerRoe = handler->getSignerAccountId(typedRoe.value(), slotLeaderId);
  if (!signerRoe) {
    return chain_tx::TxError(
        chain_err::E_TX_SIGNATURE,
        "Failed to resolve signer account id: " + signerRoe.error().message);
  }
  return signerRoe.value();
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

std::optional<std::string>
RecordHandler::userAccountMetaForRecord(const Ledger::Record &rec,
                                        uint64_t accountId) const {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return std::nullopt;
  }
  const Ledger::TypedTx &typed = typedRoe.value();
  return std::visit(
      Overloaded{
          [&](const Ledger::TxNewUser &tx) -> std::optional<std::string> {
            if (accountId == AccountBuffer::ID_GENESIS ||
                tx.toWalletId != accountId) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const Ledger::TxUserUpdate &tx) -> std::optional<std::string> {
            if (accountId == AccountBuffer::ID_GENESIS ||
                tx.walletId != accountId) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const Ledger::TxRenewal &tx) -> std::optional<std::string> {
            if (accountId == AccountBuffer::ID_GENESIS ||
                tx.walletId != accountId) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const auto &) -> std::optional<std::string> {
            return std::nullopt;
          },
      },
      typed);
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
RecordHandler::idempotencyKeyForRecord(const Ledger::Record &rec) const {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  if (rec.type >= kNumTxTypes) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  const ITxHandler *handler = get(rec.type);
  if (!handler) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return handler->getIdempotencyKey(typedRoe.value());
}

std::optional<std::string>
RecordHandler::genesisAccountMetaForRecord(const Ledger::Record &rec,
                                           const Ledger::Block &block) const {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return std::nullopt;
  }
  const Ledger::TypedTx &typed = typedRoe.value();
  return std::visit(
      Overloaded{
          [&](const Ledger::TxGenesis &tx) -> std::optional<std::string> {
            if (block.index != 0) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const Ledger::TxConfig &tx) -> std::optional<std::string> {
            return tx.meta;
          },
          [&](const Ledger::TxRenewal &tx) -> std::optional<std::string> {
            if (tx.walletId != AccountBuffer::ID_GENESIS) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const auto &) -> std::optional<std::string> {
            return std::nullopt;
          },
      },
      typed);
}

} // namespace pp

