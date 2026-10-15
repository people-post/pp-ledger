#include "ConfigPolicy.h"
#include "ErrorCodes.h"

#include <string>

namespace pp::chain_tx {

Roe<void> validateLivenessSettings(const BlockChainConfig &config) {
  if (config.heartbeatSlots > config.slotsPerEpoch) {
    return TxError(chain_err::E_TX_VALIDATION, "heartbeatSlots (" + std::to_string(config.heartbeatSlots) +
                                                   ") cannot exceed slotsPerEpoch (" +
                                                   std::to_string(config.slotsPerEpoch) + ")");
  }
  if (config.minerDueAfterSlots > 0) {
    const uint64_t minSlots =
        config.slotDuration > 0 ? (kMinMinerDueSeconds + config.slotDuration - 1) / config.slotDuration : 0;
    if (config.minerDueAfterSlots < minSlots) {
      return TxError(chain_err::E_TX_VALIDATION,
                     "minerDueAfterSlots must be 0 or at least " + std::to_string(minSlots) +
                         " (12 hours of slots): " + std::to_string(config.minerDueAfterSlots));
    }
  }
  return {};
}

} // namespace pp::chain_tx
