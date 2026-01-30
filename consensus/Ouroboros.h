#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include "Types.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pp {
namespace consensus {

/**
 * Ouroboros Consensus Protocol Implementation
 *
 * Implements the Ouroboros Proof-of-Stake consensus algorithm.
 * Key features:
 * - Slot-based block production
 * - Epoch management
 * - Stake-based slot leader selection
 * - Chain selection rules
 */
class Ouroboros : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  /**
   * Constructor
   * @param slotDuration Duration of each slot in seconds
   * @param slotsPerEpoch Number of slots in each epoch
   */
  Ouroboros();

  ~Ouroboros() override = default;

  // Stakeholder management
  void registerStakeholder(uint64_t id, uint64_t stake);
  void updateStake(uint64_t id, uint64_t newStake);
  bool removeStakeholder(uint64_t id);

  // Epoch and slot management
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  uint64_t getSlotInEpoch(uint64_t slot) const;
  int64_t getSlotStartTime(uint64_t slot) const;

  // Slot leader selection
  Roe<uint64_t> getSlotLeader(uint64_t slot) const;
  bool isSlotLeader(uint64_t slot, uint64_t stakeholderId) const;

  // Validation helpers
  bool validateSlotLeader(uint64_t slotLeader, uint64_t slot) const;
  bool validateBlockTiming(int64_t blockTimestamp, uint64_t slot) const;

  // Configuration
  void setSlotDuration(uint64_t seconds);
  void setSlotsPerEpoch(uint64_t slots);
  uint64_t getSlotDuration() const { return slotDuration_; }
  uint64_t getSlotsPerEpoch() const { return slotsPerEpoch_; }

  // Genesis time
  void setGenesisTime(int64_t timestamp);
  int64_t getGenesisTime() const { return genesisTime_; }

  // Utilities
  uint64_t getTotalStake() const;
  size_t getStakeholderCount() const;
  std::vector<Stakeholder> getStakeholders() const;

private:
  // Helper methods for slot leader selection
  uint64_t selectSlotLeader(uint64_t slot, uint64_t epoch) const;
  uint64_t calculateStakeThreshold(uint64_t stakeholderId,
                                   uint64_t totalStake) const;
  std::string hashSlotAndEpoch(uint64_t slot, uint64_t epoch) const;

  // Data members
  std::map<uint64_t, uint64_t> mStakeholders_;
  uint64_t slotDuration_{ 0 };  // Duration of each slot in seconds
  uint64_t slotsPerEpoch_{ 0 }; // Number of slots per epoch
  int64_t genesisTime_{ 0 };    // Timestamp of genesis block
};

} // namespace consensus
} // namespace pp
