#include "RecordHandler.h"

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

void RecordHandler::redirectLoggers(const std::string &baseName) {
  for (std::size_t i = 0; i < handlers_.size(); ++i) {
    if (!handlers_[i]) {
      continue;
    }
    handlers_[i]->redirectLogger(baseName + ".TxHandler." + std::to_string(i));
  }
}

} // namespace pp

