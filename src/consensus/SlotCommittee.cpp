#include "SlotCommittee.h"
#include "common/Serialize.hpp"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace pp {
namespace consensus {

SlotCommittee::SlotCommittee() {}

bool SlotCommittee::isSlotLeader(uint64_t slot,
                             uint64_t stakeholderId) const {
  auto result = getSlotLeader(slot);
  if (!result.isOk()) {
    return false;
  }

  return result.value() == stakeholderId;
}

bool SlotCommittee::isStakeUpdateNeeded() const {
  uint64_t currentEpoch = getCurrentEpoch();
  return currentEpoch != cache_.lastStakeUpdateEpoch;
}

bool SlotCommittee::isStakeUpdateNeeded(uint64_t forEpoch) const {
  return forEpoch != cache_.lastStakeUpdateEpoch;
}

bool SlotCommittee::isSlotBlockProductionTime(uint64_t slot) const {
  int64_t currentTime = getTimestamp();
  int64_t slotEndTime = getSlotEndTime(slot);
  // Block production time is within the last second of the slot
  return currentTime >= slotEndTime - 1;
}

int64_t SlotCommittee::getTimestamp() const {
  if (clockOverride_.has_value()) {
    return *clockOverride_;
  }
  auto now = std::chrono::system_clock::now();
  int64_t localTime =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  return localTime + config_.timeOffset;
}

uint64_t SlotCommittee::getCurrentSlot() const {
  return getSlotFromTimestamp(getTimestamp());
}

uint64_t SlotCommittee::getSlotFromTimestamp(int64_t timestamp) const {
  assertConfigIsSet();
  if (timestamp < config_.genesisTime) {
    return 0;
  }
  int64_t elapsed = timestamp - config_.genesisTime;
  return static_cast<uint64_t>(elapsed / config_.slotDuration);
}

uint64_t SlotCommittee::getCurrentEpoch() const {
  uint64_t slot = getCurrentSlot();
  if (config_.slotsPerEpoch == 0) {
    log().error << "Slots per epoch is 0";
  }
  return slot / config_.slotsPerEpoch;
}

uint64_t SlotCommittee::getSlotInEpoch(uint64_t slot) const {
  return slot % config_.slotsPerEpoch;
}

int64_t SlotCommittee::getSlotStartTime(uint64_t slot) const {
  return config_.genesisTime + static_cast<int64_t>(slot * config_.slotDuration);
}

int64_t SlotCommittee::getSlotEndTime(uint64_t slot) const {
  return getSlotStartTime(slot) + static_cast<int64_t>(config_.slotDuration);
}

SlotCommittee::Roe<uint64_t> SlotCommittee::getSlotLeader(uint64_t slot) const {
  if (cache_.mStakeholders.empty()) {
    return Error(1, "No stakeholders registered");
  }

  uint64_t epoch = getEpochFromSlot(slot);
  auto forced = forcedLeaders_.find(slot);
  if (forced == forcedLeaders_.end() && !hasEpochSeedFor(epoch)) {
    return Error(2, "Epoch seed not set for epoch " + std::to_string(epoch));
  }

  uint64_t leader = selectSlotLeader(slot, epoch);
  return leader;
}

uint64_t SlotCommittee::getEpochFromSlot(uint64_t slot) const {
  if (config_.slotsPerEpoch == 0) {
    log().error << "Slots per epoch is 0";
  }
  return slot / config_.slotsPerEpoch;
}

uint64_t SlotCommittee::getStake(uint64_t stakeholderId) const {
  auto it = cache_.mStakeholders.find(stakeholderId);
  if (it == cache_.mStakeholders.end()) {
    return 0;
  }
  return it->second;
}

uint64_t SlotCommittee::getTotalStake() const {
  return std::accumulate(
      cache_.mStakeholders.begin(), cache_.mStakeholders.end(), uint64_t(0),
      [](uint64_t sum, const auto &pair) { return sum + pair.second; });
}

size_t SlotCommittee::getStakeholderCount() const { return cache_.mStakeholders.size(); }

std::vector<Stakeholder> SlotCommittee::getStakeholders() const {
  std::vector<Stakeholder> result;
  result.reserve(cache_.mStakeholders.size());

  for (const auto &[id, stake] : cache_.mStakeholders) {
    result.emplace_back();
    result.back().id = id;
    result.back().stake = stake;
  }

  return result;
}

void SlotCommittee::assertConfigIsSet() const {
  if (config_.slotDuration == 0) {
    log().error << "Config is not set. Slot duration is 0";
    throw std::runtime_error("Config is not set. Slot duration is 0");
  }
  if (config_.slotsPerEpoch == 0) {
    log().error << "Config is not set. Slots per epoch is 0";
    throw std::runtime_error("Config is not set. Slots per epoch is 0");
  }
}

void SlotCommittee::init(const Config& config) {
  config_ = config;
  cache_ = {};
  clockOverride_.reset();
  forcedLeaders_.clear();
  clearEpochSeed();
}

void SlotCommittee::setClockOverride(std::optional<int64_t> unixSeconds) {
  clockOverride_ = unixSeconds;
}

void SlotCommittee::forceSlotLeader(uint64_t slot, uint64_t stakeholderId) {
  forcedLeaders_[slot] = stakeholderId;
}

void SlotCommittee::clearForcedSlotLeaders() { forcedLeaders_.clear(); }

bool SlotCommittee::hasEpochSeedFor(uint64_t epoch) const {
  return epochSeed_.size() == utl::SHA256_DIGEST_SIZE &&
         epochSeedEpoch_ == epoch;
}

void SlotCommittee::setEpochSeed(uint64_t epoch, const std::string &seed32) {
  if (seed32.size() != utl::SHA256_DIGEST_SIZE) {
    log().error << "setEpochSeed: seed must be "
                << utl::SHA256_DIGEST_SIZE << " bytes";
    throw std::invalid_argument("epoch seed must be 32 bytes");
  }
  epochSeed_ = seed32;
  epochSeedEpoch_ = epoch;
}

void SlotCommittee::clearEpochSeed() {
  epochSeed_.clear();
  epochSeedEpoch_ = 0;
}

void SlotCommittee::setStakeholders(const std::vector<Stakeholder>& stakeholders) {
  setStakeholders(stakeholders, getCurrentEpoch());
}

void SlotCommittee::setStakeholders(const std::vector<Stakeholder>& stakeholders,
                                uint64_t forEpoch) {
  cache_.mStakeholders.clear();
  for (const auto& stakeholder : stakeholders) {
    cache_.mStakeholders[stakeholder.id] = stakeholder.stake;
  }
  cache_.lastStakeUpdateEpoch = forEpoch;
}

std::vector<uint64_t> SlotCommittee::getEligibleLeaderPool() const {
  if (cache_.mStakeholders.empty()) {
    return {};
  }
  std::vector<std::pair<uint64_t, uint64_t>> byStake(cache_.mStakeholders.begin(),
                                                     cache_.mStakeholders.end());
  byStake.erase(
      std::remove_if(byStake.begin(), byStake.end(),
                     [](const auto& p) { return p.second == 0; }),
      byStake.end());
  if (byStake.empty()) {
    return {};
  }
  // Sort by stake descending, then by id ascending for deterministic tie-break
  std::sort(byStake.begin(), byStake.end(),
            [](const auto& a, const auto& b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });
  size_t n = std::min(kMaxLeaderPoolSize, byStake.size());
  std::vector<uint64_t> pool;
  pool.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    pool.push_back(byStake[i].first);
  }
  return pool;
}

uint64_t SlotCommittee::selectSlotLeader(uint64_t slot, uint64_t epoch) const {
  auto forced = forcedLeaders_.find(slot);
  if (forced != forcedLeaders_.end()) {
    return forced->second;
  }

  std::vector<uint64_t> pool = getEligibleLeaderPool();
  if (pool.empty()) {
    return 0;
  }

  std::string slotHash = hashSlotElection(slot, epoch, epochSeed_);
  if (slotHash.size() < 8) {
    return pool[0];
  }

  // First 8 bytes of raw SHA-256 as big-endian u64; equal weight in pool.
  uint64_t hashValue = 0;
  for (size_t i = 0; i < 8; ++i) {
    hashValue = (hashValue << 8) |
                static_cast<uint64_t>(static_cast<unsigned char>(slotHash[i]));
  }
  size_t index = static_cast<size_t>(hashValue % pool.size());
  return pool[index];
}

std::string SlotCommittee::hashSlotElection(uint64_t slot, uint64_t epoch,
                                            const std::string &epochSeed) const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  const std::string domain = "pp-ledger/slot-committee/v2";
  oss.write(domain.data(), static_cast<std::streamsize>(domain.size()));
  ar & slot & epoch;
  oss.write(epochSeed.data(), static_cast<std::streamsize>(epochSeed.size()));
  return utl::sha256Raw(oss.str());
}

bool SlotCommittee::validateSlotLeader(uint64_t slotLeader,
                                   uint64_t slot) const {
  auto result = getSlotLeader(slot);
  if (!result.isOk()) {
    return false;
  }
  return slotLeader == result.value();
}

bool SlotCommittee::validateBlockTiming(int64_t blockTimestamp, uint64_t slot) const {
  int64_t slotStart = getSlotStartTime(slot);
  int64_t slotEnd = slotStart + static_cast<int64_t>(config_.slotDuration);

  return blockTimestamp >= slotStart && blockTimestamp < slotEnd;
}

} // namespace consensus
} // namespace pp
