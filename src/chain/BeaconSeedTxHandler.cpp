#include "chain/BeaconSeedTxHandler.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/ErrorCodes.h"
#include "chain/TxTyped.h"
#include "ledger/chain/Types.h"
#include "ledger/consensus/EpochSeed.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t> BeaconSeedTxHandler::getSignerAccountId(const Ledger::TypedTx& tx,
                                                                uint64_t /*slotLeaderId*/) const {
  // Indexed under the genesis account; the signature is the beacon key's.
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconSeed>(tx, "getSignerAccountId", "TxBeaconSeed");
  if (!pRoe) {
    return pRoe.error();
  }
  return AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>> BeaconSeedTxHandler::getIdempotencyKey(
    const Ledger::TypedTx& tx) const {
  // One reveal per epoch is its own replay guard.
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconSeed>(tx, "getIdempotencyKey", "TxBeaconSeed");
  if (!pRoe) {
    return pRoe.error();
  }
  return std::optional<std::pair<uint64_t, uint64_t>>{};
}

chain_tx::Roe<size_t> BeaconSeedTxHandler::getBillableCustomMetaSizeForFee(const BlockChainConfig& /*config*/,
                                                                           const Ledger::TypedTx& /*tx*/) const {
  return size_t{0};
}

chain_tx::Roe<void> BeaconSeedTxHandler::check(const Ledger::TxBeaconSeed& tx, const TxContext& ctx,
                                               uint64_t slot) const {
  const uint64_t epoch = ctx.consensus.getEpochFromSlot(slot);
  if (tx.epoch != epoch + 1) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "A beacon seed reveal for epoch " + std::to_string(tx.epoch) +
                                                             " is published in the epoch before it, not in epoch " +
                                                             std::to_string(epoch));
  }
  if (!ctx.optAuthority.has_value()) {
    return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain authority not initialized");
  }
  const BeaconCertificate& cert = ctx.optAuthority->beacon;
  if (!cert.hasSeedChain()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "The beacon certificate in force has no seed chain");
  }
  if (tx.epoch <= cert.seedAnchorEpoch || tx.epoch - cert.seedAnchorEpoch > cert.seedChainLength) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "The beacon's seed chain does not cover epoch " + std::to_string(tx.epoch));
  }
  // Complete for a node that checks this: it replays from a checkpoint and turns Full at the next,
  // both the first block of their epoch (checkpointEligible), so it has applied every earlier block
  // of this epoch — any reveal for tx.epoch included.
  if (ctx.beaconReveals.count(tx.epoch) != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "A beacon seed reveal for epoch " + std::to_string(tx.epoch) + " is already recorded");
  }
  // From the last reveal on this same chain when there is one: fewer steps, the same answer (that
  // reveal lies on the anchor's chain).
  std::string target = cert.seedAnchor;
  uint64_t steps = tx.epoch - cert.seedAnchorEpoch;
  const auto& chain = ctx.beaconRevealChain;
  auto last = ctx.beaconReveals.lower_bound(tx.epoch);
  if (chain.has_value() && chain->anchor == cert.seedAnchor && chain->anchorEpoch == cert.seedAnchorEpoch &&
      last != ctx.beaconReveals.begin()) {
    --last;
    if (last->first >= chain->fromEpoch && last->first > cert.seedAnchorEpoch) {
      target = last->second;
      steps = tx.epoch - last->first;
    }
  }
  if (!consensus::verifyBeaconSeedReveal(target, steps, tx.reveal)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "The beacon seed reveal for epoch " +
                                                             std::to_string(tx.epoch) +
                                                             " does not match the certificate's seed chain");
  }
  return {};
}

chain_tx::Roe<void> BeaconSeedTxHandler::applyBuffer(const Ledger::TypedTx& tx, AccountBuffer& bank,
                                                     const BufferApplyContext& c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconSeed>(tx, "applyBuffer", "TxBeaconSeed");
  if (!pRoe) {
    return pRoe.error();
  }
  // Reveals are recorded only when a block applies: a second one would pass here. A draft's records
  // share one epoch, so one tag covers it.
  if (bank.hasDraftTag(kDraftTag)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "A block holds at most one beacon seed reveal");
  }
  if (auto valid = check(*pRoe.value(), c.ctx, c.effectiveSlot); !valid) {
    return valid;
  }
  bank.addDraftTag(kDraftTag);
  return {};
}

chain_tx::Roe<void> BeaconSeedTxHandler::applyBlock(const Ledger::TypedTx& tx, AccountBuffer& /*bank*/,
                                                    const BlockApplyContext& c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconSeed>(tx, "applyBlock", "TxBeaconSeed");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto& seed = *pRoe.value();
  TxContext& ctx = c.ctx;
  if (chain_block::admissionTxStrict(c.admissionMode)) {
    if (auto valid = check(seed, ctx, c.blockSlot); !valid) {
      return valid;
    }
  }
  // CheckpointReplay records it unchecked: the block source is trusted there.
  ctx.beaconReveals[seed.epoch] = seed.reveal;
  if (ctx.optAuthority.has_value()) {
    const BeaconCertificate& cert = ctx.optAuthority->beacon;
    auto& chain = ctx.beaconRevealChain;
    if (!chain.has_value() || chain->anchor != cert.seedAnchor || chain->anchorEpoch != cert.seedAnchorEpoch) {
      chain = BeaconRevealChain{cert.seedAnchor, cert.seedAnchorEpoch, seed.epoch};
    }
  }
  return {};
}

} // namespace pp
