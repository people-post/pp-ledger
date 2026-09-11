#ifndef PP_LEDGER_CHAIN_TYPES_H
#define PP_LEDGER_CHAIN_TYPES_H

#include "../client/Client.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace pp {

struct Checkpoint {
  uint64_t lastId{0};
  uint64_t currentId{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &lastId &currentId;
  }
};

struct CheckpointConfig {
  uint64_t minBlocks{0};
  uint64_t minAgeSeconds{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &minBlocks &minAgeSeconds;
  }
};

struct BlockChainConfig {
  int64_t genesisTime{0};
  uint64_t slotDuration{0};
  uint64_t slotsPerEpoch{0};
  uint64_t maxCustomMetaSize{0};
  uint64_t maxTransactionsPerBlock{0};
  std::vector<uint16_t> minFeeCoefficients;
  uint32_t freeCustomMetaSize{0};
  CheckpointConfig checkpoint;
  uint64_t maxValidationTimespanSeconds{0};
  /** Chain identity included in every transaction signing message. */
  std::string networkId;
  /**
   * Empty-block heartbeat: when the mempool/renewals are idle, a slot leader
   * may seal an empty block if `currentSlot - tip.slot >= heartbeatSlots`.
   * `0` disables empty seals (work-only production).
   */
  uint64_t heartbeatSlots{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &genesisTime &slotDuration &slotsPerEpoch &maxCustomMetaSize
        &maxTransactionsPerBlock &minFeeCoefficients &freeCustomMetaSize
            &checkpoint &maxValidationTimespanSeconds &networkId
                &heartbeatSlots;
  }
};

/**
 * Miner empty-seal policy (genesis `heartbeatSlots`).
 * Requires `currentSlot >= tipSlot` and lag of at least `heartbeatSlots`.
 * `heartbeatSlots == 0` disables.
 */
inline bool shouldSealEmptyHeartbeat(uint64_t currentSlot, uint64_t tipSlot,
                                     uint64_t heartbeatSlots) {
  if (heartbeatSlots == 0) {
    return false;
  }
  if (currentSlot < tipSlot) {
    return false;
  }
  return (currentSlot - tipSlot) >= heartbeatSlots;
}

struct GenesisAccountMeta {
  constexpr static const uint32_t VERSION = 3;

  BlockChainConfig config;
  Client::UserAccount genesis;

  template <typename Archive> void serialize(Archive &ar) {
    ar &config &genesis;
  }

  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);
};

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config);
std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config);

} // namespace pp

#endif
