#ifndef PP_LEDGER_TX_FORWARD_POLICY_H
#define PP_LEDGER_TX_FORWARD_POLICY_H

#include <cstdint>

namespace pp {

/** What a miner does with a T_REQ_TX_FORWARD it receives. */
enum class TxForwardAction {
  AddToPool,      ///< We lead the target slot and it is current.
  HoldForSlot,    ///< We lead the target slot; it has not started yet.
  CacheBehind,    ///< Our chain is behind the sender's: schedule may be provisional.
  CacheNotLeader, ///< Not ours (or already past): cache for the next slot's retry.
};

/**
 * Receiver rule for forwarded transactions. Never "forward on now": cached
 * transactions only move on through the per-slot retry, which targets a later
 * slot, so a forward cannot bounce between miners however their clocks skew.
 */
inline TxForwardAction decideTxForward(uint64_t tipEpoch, uint64_t senderTipEpoch, uint64_t currentSlot,
                                       uint64_t targetSlot, bool leadsTargetSlot) {
  if (tipEpoch < senderTipEpoch) {
    return TxForwardAction::CacheBehind;
  }
  if (leadsTargetSlot && targetSlot >= currentSlot) {
    return targetSlot == currentSlot ? TxForwardAction::AddToPool : TxForwardAction::HoldForSlot;
  }
  return TxForwardAction::CacheNotLeader;
}

} // namespace pp

#endif // PP_LEDGER_TX_FORWARD_POLICY_H
