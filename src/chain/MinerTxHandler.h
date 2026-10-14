#ifndef PP_LEDGER_MINER_TX_HANDLER_H
#define PP_LEDGER_MINER_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

/**
 * T_MINER_REGISTER / T_MINER_REVOKE (docs/contracts/WIRE_SCHEMA.md § Miner registration).
 * Signed by the account itself. They change only the account's miner state, in place: the
 * account's blockId (where renewal reads its meta) does not move.
 *
 * Register: not a system account, not registered, not in its cool-down, balance after the fee
 * at least `minerMinStake`; fee = meta-size fee + `minerRegistrationFee`. Revoke: registered;
 * meta-size fee. Either takes effect in the committee at the next epoch's stake snapshot.
 */
class MinerTxHandler : public ITxHandler {
public:
  explicit MinerTxHandler(bool registering) : registering_(registering) {}

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
  chain_tx::Roe<const Ledger::TxMiner *> expect(const Ledger::TypedTx &tx) const;
  chain_tx::Roe<void> applyMiner(const Ledger::TxMiner &tx, const TxContext &ctx, AccountBuffer &bank,
                                 uint64_t slot, chain_block::BlockAdmissionMode admissionMode) const;
  chain_tx::Roe<void> checkRegisterAllowed(const Ledger::TxMiner &tx, const TxContext &ctx,
                                           const Client::Wallet &wallet, uint64_t slot) const;

  bool registering_;
};

} // namespace pp

#endif
