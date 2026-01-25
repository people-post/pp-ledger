#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
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
  struct StakeholderInfo {
    std::string id;
    uint64_t stake;

    StakeholderInfo(const std::string &stakeholderId, uint64_t stakeAmount)
        : id(stakeholderId), stake(stakeAmount) {}
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  /**
   * Constructor
   * @param slotDuration Duration of each slot in seconds
   * @param slotsPerEpoch Number of slots in each epoch
   */
  explicit Ouroboros(uint64_t slotDuration = 1, uint64_t slotsPerEpoch = 21600);

  ~Ouroboros() override = default;

  // Stakeholder management
  void registerStakeholder(const std::string &id, uint64_t stake);
  void updateStake(const std::string &id, uint64_t newStake);
  bool removeStakeholder(const std::string &id);

  // Epoch and slot management
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  uint64_t getSlotInEpoch(uint64_t slot) const;
  int64_t getSlotStartTime(uint64_t slot) const;

  // Slot leader selection
  Roe<std::string> getSlotLeader(uint64_t slot) const;
  bool isSlotLeader(uint64_t slot, const std::string &stakeholderId) const;

  // Validation helpers
  bool validateSlotLeader(const std::string &slotLeader, uint64_t slot) const;
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
  std::vector<StakeholderInfo> getStakeholders() const;

private:
  // Helper methods for slot leader selection
  std::string selectSlotLeader(uint64_t slot, uint64_t epoch) const;
  uint64_t calculateStakeThreshold(const std::string &stakeholderId,
                                   uint64_t totalStake) const;
  std::string hashSlotAndEpoch(uint64_t slot, uint64_t epoch) const;

  // Data members
  std::map<std::string, uint64_t> mStakeholders_;
  uint64_t slotDuration_;  // Duration of each slot in seconds
  uint64_t slotsPerEpoch_; // Number of slots per epoch
  int64_t genesisTime_;    // Timestamp of genesis block
};

} // namespace consensus
} // namespace pp
