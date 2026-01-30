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
  struct Config {
    int64_t genesisTime{ 0 };    // timestamp of genesis block
    int64_t timeOffset{ 0 };     // beacon_time = local_time + timeOffset_
    uint64_t slotDuration{ 0 };  // duration of each slot in seconds
    uint64_t slotsPerEpoch{ 0 }; // number of slots in each epoch
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
  Ouroboros();

  ~Ouroboros() override = default;

  // ----- accessors -----
  bool isSlotLeader(uint64_t slot, uint64_t stakeholderId) const;

  const Config& getConfig() const { return config_; }
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  uint64_t getSlotInEpoch(uint64_t slot) const;
  int64_t getSlotStartTime(uint64_t slot) const;
  int64_t getSlotEndTime(uint64_t slot) const;
  uint64_t getTotalStake() const;
  size_t getStakeholderCount() const;
  std::vector<Stakeholder> getStakeholders() const;
  Roe<uint64_t> getSlotLeader(uint64_t slot) const;

  // ----- methods -----
  void init(const Config& config);
  bool validateSlotLeader(uint64_t slotLeader, uint64_t slot) const;
  bool validateBlockTiming(int64_t blockTimestamp, uint64_t slot) const;

  void registerStakeholder(uint64_t id, uint64_t stake);
  void updateStake(uint64_t id, uint64_t newStake);
  bool removeStakeholder(uint64_t id);

private:
  // Helper methods for slot leader selection
  uint64_t getEpochFromSlot(uint64_t slot) const;
  uint64_t selectSlotLeader(uint64_t slot, uint64_t epoch) const;
  uint64_t calculateStakeThreshold(uint64_t stakeholderId,
                                   uint64_t totalStake) const;
  std::string hashSlotAndEpoch(uint64_t slot, uint64_t epoch) const;

  // Data members
  std::map<uint64_t, uint64_t> mStakeholders_;
  Config config_;
};

} // namespace consensus
} // namespace pp
