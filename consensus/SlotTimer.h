#pragma once

#include "Module.h"
#include <cstdint>

namespace pp {
namespace consensus {

/**
 * Slot Timer
 *
 * Manages timing and synchronization for slot-based consensus.
 * Provides utilities for:
 * - Waiting for specific slots
 * - Checking if current time is within a slot
 * - Calculating time until next slot
 */
class SlotTimer : public Module {
public:
  explicit SlotTimer(uint64_t slotDuration = 1);
  ~SlotTimer() override = default;

  /**
   * Get current slot number based on genesis time
   */
  uint64_t getCurrentSlot(int64_t genesisTime) const;

  /**
   * Get absolute time for start of a slot
   */
  int64_t getSlotStartTime(uint64_t slot, int64_t genesisTime) const;

  /**
   * Get absolute time for end of a slot
   */
  int64_t getSlotEndTime(uint64_t slot, int64_t genesisTime) const;

  /**
   * Check if given timestamp falls within a slot
   */
  bool isTimeInSlot(int64_t timestamp, uint64_t slot,
                    int64_t genesisTime) const;

  /**
   * Calculate time remaining in current slot (in seconds)
   */
  int64_t getTimeUntilNextSlot(int64_t genesisTime) const;

  /**
   * Calculate time until a specific slot starts (in seconds)
   * Returns negative if slot has already started
   */
  int64_t getTimeUntilSlot(uint64_t slot, int64_t genesisTime) const;

  /**
   * Get current timestamp
   */
  int64_t getCurrentTime() const;

  // Configuration
  void setSlotDuration(uint64_t duration);
  uint64_t getSlotDuration() const { return slotDuration_; }

private:
  uint64_t slotDuration_;
};

} // namespace consensus
} // namespace pp
