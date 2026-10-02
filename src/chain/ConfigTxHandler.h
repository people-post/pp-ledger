#ifndef PP_LEDGER_CONFIG_TX_HANDLER_H
#define PP_LEDGER_CONFIG_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class ConfigTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<bool>
  matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const override;

  chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
  getIdempotencyKey(const Ledger::TypedTx &tx) const override;

  chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

  std::optional<std::string>
  getGenesisAccountMetaForTx(const Ledger::TypedTx &tx,
                             const Ledger::Block &block) const override;

private:
  /**
   * Validate the update and replace the genesis account (keys apply at once);
   * returns the new chain config, which the caller schedules for the next
   * epoch boundary. `epoch` is the epoch the update is applied in.
   */
  chain_tx::Roe<BlockChainConfig>
  applyConfigUpdate(const Ledger::TxConfig &tx, const TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId, uint64_t epoch,
                    chain_block::BlockAdmissionMode admissionMode) const;
};
} // namespace pp

#endif
