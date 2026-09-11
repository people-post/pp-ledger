#pragma once

#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "EpochSeed.h"
#include "Types.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pp {
namespace consensus {

/**
 * SlotCommittee — live slot/epoch schedule and leader election for pp-ledger.
 *
 * Designed behavior (not classic Ouroboros / stake-weighted VRF):
 * - Fixed slots and epochs from genesis config
 * - Stakeholder cache refreshed per epoch from native balances
 * - Eligible committee = all positive-stake accounts if ≤ N, else top N by stake
 * - Equal-weight lottery within that committee:
 *   SHA-256("pp-ledger/slot-committee/v2" || slot || epoch || epochSeed)
 * - Beacon-centered chain authority; this type schedules proposers, it does not
 *   implement multi-beacon BFT or fork choice
 *
 * Ouroboros remains a literature reference only (see docs/contracts/WIRE_SCHEMA.md).
 */
class SlotCommittee : public Module {
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

  SlotCommittee();

  ~SlotCommittee() override = default;

  // ----- accessors -----
  bool isSlotLeader(uint64_t slot, uint64_t stakeholderId) const;
  /** True when live clock epoch differs from last update (for live adding). */
  bool isStakeUpdateNeeded() const;
  /** True when given epoch differs from last update (for load-from-ledger). */
  bool isStakeUpdateNeeded(uint64_t forEpoch) const;
  bool isSlotBlockProductionTime(uint64_t slot) const;

  const Config& getConfig() const { return config_; }
  uint64_t getCurrentSlot() const;
  /** Slot that contains the given timestamp (same formula as getCurrentSlot but for arbitrary time). */
  uint64_t getSlotFromTimestamp(int64_t timestamp) const;
  uint64_t getCurrentEpoch() const;
  /** Epoch index for a slot (for load-from-ledger). */
  uint64_t getEpochFromSlot(uint64_t slot) const;
  uint64_t getSlotInEpoch(uint64_t slot) const;
  int64_t getSlotStartTime(uint64_t slot) const;
  int64_t getSlotEndTime(uint64_t slot) const;
  uint64_t getTotalStake() const;
  uint64_t getStake(uint64_t stakeholderId) const;
  size_t getStakeholderCount() const;
  std::vector<Stakeholder> getStakeholders() const;
  Roe<uint64_t> getSlotLeader(uint64_t slot) const;
  int64_t getTimestamp() const;

  /** Active epoch seed (32 raw bytes) for leader election; empty if unset. */
  const std::string &getEpochSeed() const { return epochSeed_; }
  /** Epoch that `getEpochSeed()` applies to; undefined if seed empty. */
  uint64_t getEpochSeedEpoch() const { return epochSeedEpoch_; }
  bool hasEpochSeedFor(uint64_t epoch) const;

  /** Set stakeholders and record update epoch (live: use getCurrentEpoch()). */
  void setStakeholders(const std::vector<Stakeholder>& stakeholders);
  /** Set stakeholders for a specific epoch (load-from-ledger: use block slot). */
  void setStakeholders(const std::vector<Stakeholder>& stakeholders,
                       uint64_t forEpoch);

  // ----- methods -----
  void init(const Config& config);
  bool validateSlotLeader(uint64_t slotLeader, uint64_t slot) const;
  bool validateBlockTiming(int64_t blockTimestamp, uint64_t slot) const;

  /**
   * Install the lottery seed for `epoch` (must be 32 raw bytes).
   * Production: Chain derives and sets; tests may call directly.
   */
  void setEpochSeed(uint64_t epoch, const std::string &seed32);
  void clearEpochSeed();

  /**
   * Test/sim injectors — pin wall clock and/or override elected leaders.
   * Production binaries must not call these; reserved for unit tests and
   * in-process smoke compose (docs/architecture/TESTING.md).
   */
  void setClockOverride(std::optional<int64_t> unixSeconds);
  void forceSlotLeader(uint64_t slot, uint64_t stakeholderId);
  void clearForcedSlotLeaders();

private:
  struct Cache {
    std::map<uint64_t, uint64_t> mStakeholders;
    uint64_t lastStakeUpdateEpoch{ static_cast<uint64_t>(-1) }; // -1 = never
  };

  void assertConfigIsSet() const;
  /**
   * Eligible committee: all positive-stake ids if ≤ kMaxLeaderPoolSize,
   * else top N by stake (id ascending tie-break). Equal weight inside the pool.
   */
  std::vector<uint64_t> getEligibleLeaderPool() const;
  uint64_t selectSlotLeader(uint64_t slot, uint64_t epoch) const;
  std::string hashSlotElection(uint64_t slot, uint64_t epoch,
                               const std::string &epochSeed) const;

  static constexpr size_t kMaxLeaderPoolSize = 100;

  Config config_;
  Cache cache_;
  std::optional<int64_t> clockOverride_;
  std::map<uint64_t, uint64_t> forcedLeaders_;  // slot -> stakeholderId
  std::string epochSeed_;
  uint64_t epochSeedEpoch_{ 0 };
};

} // namespace consensus
} // namespace pp
