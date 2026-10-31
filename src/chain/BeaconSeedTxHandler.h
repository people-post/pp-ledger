#ifndef PP_LEDGER_BEACON_SEED_TX_HANDLER_H
#define PP_LEDGER_BEACON_SEED_TX_HANDLER_H

#include "ledger/chain/ITxHandler.h"

namespace pp {

/**
 * T_BEACON_SEED (beacon epoch seed): during epoch E-1 the beacon reveals the step of its seed hash
 * chain for epoch E, which the certificate in force commits to (seedAnchor). The record is signed by
 * the beacon key, not by an account (the chain checks that before apply). One reveal per epoch; a
 * block holds at most one. Applied, the reveal joins TxContext::beaconReveals.
 */
class BeaconSeedTxHandler final : public ITxHandler {
public:
  /** Draft tag: one reveal per block (AccountBuffer::addDraftTag). */
  static constexpr const char *kDraftTag = "beacon-seed";

  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
  getIdempotencyKey(const Ledger::TypedTx &tx) const override;

  chain_tx::Roe<size_t>
  getBillableCustomMetaSizeForFee(const BlockChainConfig &config, const Ledger::TypedTx &tx) const override;

  chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

private:
  /** Strict checks shared by draft and block apply, for a record in a block at `slot`. */
  chain_tx::Roe<void> check(const Ledger::TxBeaconSeed &tx, const TxContext &ctx, uint64_t slot) const;
};

} // namespace pp

#endif
