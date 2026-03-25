#ifndef PP_LEDGER_CONFIG_TX_HANDLER_H
#define PP_LEDGER_CONFIG_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class ConfigTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::Transaction &tx, logging::Logger &logger,
                    const std::optional<BlockChainConfig> &chainConfigBaseline,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
                    bool commitOptChainConfig,
                    std::optional<BlockChainConfig> *commitTarget) override;
};

} // namespace pp

#endif
