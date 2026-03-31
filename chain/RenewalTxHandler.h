#ifndef PP_LEDGER_RENEWAL_TX_HANDLER_H
#define PP_LEDGER_RENEWAL_TX_HANDLER_H

#include "UserAccountUpsertBase.h"

namespace pp {

/** T_RENEWAL when renewing the genesis account (miner-signed). */
class RenewalTxHandler final : public UserAccountUpsertBase {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  bool isRenewalTx() const override { return true; }
  bool participatesInAccountRenewalValidation() const override { return true; }

  chain_tx::Roe<bool>
  matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const override;

  chain_tx::Roe<std::optional<uint64_t>>
  getRenewalAccountIdIfAny(const Ledger::TypedTx &tx) const override;

  chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

  std::optional<std::string>
  getUserAccountMetaForTx(const Ledger::TypedTx &tx,
                          uint64_t accountId) const override;

  std::optional<std::string>
  getGenesisAccountMetaForTx(const Ledger::TypedTx &tx,
                             const Ledger::Block &block) const override;

  chain_tx::Roe<size_t>
  getBillableCustomMetaSizeForFee(const BlockChainConfig &config,
                                  const Ledger::TypedTx &tx) const override;

private:
  chain_tx::Roe<void>
  applyRenewal(const Ledger::TxRenewal &tx, const TxContext &ctx,
               AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
               bool isStrictMode) const;
};

} // namespace pp

#endif
