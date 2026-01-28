#pragma once

#include "Module.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pp {
namespace consensus {

/**
 * Epoch Manager
 *
 * Manages epoch transitions, slot assignments, and epoch-specific state.
 * In Ouroboros:
 * - Time is divided into epochs
 * - Each epoch contains a fixed number of slots
 * - Slot leaders are determined at the beginning of each epoch
 */
class EpochManager : public Module {
public:
  struct EpochInfo {
    uint64_t number{ 0 };
    int64_t startTime{ 0 };
    int64_t endTime{ 0 };
    uint64_t startSlot{ 0 };
    uint64_t endSlot{ 0 };
    std::string nonce;
    std::map<uint64_t, std::string> slotLeaders; // slot -> leader mapping
  };

  /**
   * Constructor
   * @param slotsPerEpoch Number of slots in each epoch
   * @param slotDuration Duration of each slot in seconds
   */
  EpochManager();

  ~EpochManager() override = default;

  // Epoch operations
  void initializeEpoch(uint64_t epochNumber, const std::string &nonce);
  void finalizeEpoch(uint64_t epochNumber,
                     const std::vector<std::string> &blockHashes);

  // Epoch queries
  EpochInfo getEpochInfo(uint64_t epochNumber) const;
  EpochInfo getCurrentEpochInfo() const;
  uint64_t getCurrentEpoch() const;
  bool isEpochInitialized(uint64_t epochNumber) const;

  // Slot leader management
  void setSlotLeader(uint64_t epochNumber, uint64_t slot,
                     const std::string &leader);
  std::string getSlotLeader(uint64_t epochNumber, uint64_t slot) const;

  // Configuration
  void setGenesisTime(int64_t timestamp);
  int64_t getGenesisTime() const { return genesisTime_; }
  void setSlotsPerEpoch(uint64_t slots);
  uint64_t getSlotsPerEpoch() const { return slotsPerEpoch_; }
  void setSlotDuration(uint64_t duration);
  uint64_t getSlotDuration() const { return slotDuration_; }

  // Slot utilities
  uint64_t getCurrentSlot() const;
  uint64_t getEpochFromSlot(uint64_t slot) const;
  uint64_t getSlotInEpoch(uint64_t slot) const;
  int64_t getSlotStartTime(uint64_t slot) const;
  int64_t getSlotEndTime(uint64_t slot) const;

private:
  uint64_t slotsPerEpoch_{ 0 };
  uint64_t slotDuration_{ 0 };
  int64_t genesisTime_{ 0 };
  std::map<uint64_t, EpochInfo> epochs_;
  mutable uint64_t cachedCurrentEpoch_{ 0 };
  mutable int64_t lastUpdateTime_{ 0 };
};

} // namespace consensus
} // namespace pp
