#ifndef PP_LEDGER_DOMAIN_ASSIGN_TX_HANDLER_H
#define PP_LEDGER_DOMAIN_ASSIGN_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

/**
 * T_DOMAIN_ASSIGN (projects/domains): the registrar gives an issued account its domain — one it has
 * none of yet, a domain of 4–7 letters bound to no account. Signed by the
 * registrar, which pays the fee (meta-size). Applied in place, as miner state: the account's blockId
 * does not move.
 */
class DomainAssignTxHandler final : public ITxHandler {
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

private:
  chain_tx::Roe<void> apply(const Ledger::TxDomainAssign &tx, const TxContext &ctx, AccountBuffer &bank,
                            const AccountBuffer *committed, uint64_t blockId,
                            chain_block::BlockAdmissionMode admissionMode) const;
};

} // namespace pp

#endif
