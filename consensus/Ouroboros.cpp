#include "Ouroboros.h"
#include "Utilities.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace pp {
namespace consensus {

Ouroboros::Ouroboros() {}

bool Ouroboros::isSlotLeader(uint64_t slot,
                             uint64_t stakeholderId) const {
  auto result = getSlotLeader(slot);
  if (!result.isOk()) {
    return false;
  }

  return result.value() == stakeholderId;
}

bool Ouroboros::isStakeUpdateNeeded() const {
  uint64_t currentEpoch = getCurrentEpoch();
  return currentEpoch != cache_.lastStakeUpdateEpoch;
}

bool Ouroboros::isStakeUpdateNeeded(uint64_t forEpoch) const {
  return forEpoch != cache_.lastStakeUpdateEpoch;
}

bool Ouroboros::isSlotBlockProductionTime(uint64_t slot) const {
  int64_t currentTime = getTimestamp();
  int64_t slotEndTime = getSlotEndTime(slot);
  // Block production time is within the last second of the slot
  return currentTime >= slotEndTime - 1;
}

int64_t Ouroboros::getTimestamp() const {
  auto now = std::chrono::system_clock::now();
  int64_t localTime =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  return localTime + config_.timeOffset;
}

uint64_t Ouroboros::getCurrentSlot() const {
  return getSlotFromTimestamp(getTimestamp());
}

uint64_t Ouroboros::getSlotFromTimestamp(int64_t timestamp) const {
  if (timestamp < config_.genesisTime) {
    return 0;
  }
  int64_t elapsed = timestamp - config_.genesisTime;
  if (config_.slotDuration == 0) {
    log().error << "Slot duration is 0";
  }
  return static_cast<uint64_t>(elapsed / config_.slotDuration);
}

uint64_t Ouroboros::getCurrentEpoch() const {
  uint64_t slot = getCurrentSlot();
  if (config_.slotsPerEpoch == 0) {
    log().error << "Slots per epoch is 0";
  }
  return slot / config_.slotsPerEpoch;
}

uint64_t Ouroboros::getSlotInEpoch(uint64_t slot) const {
  return slot % config_.slotsPerEpoch;
}

int64_t Ouroboros::getSlotStartTime(uint64_t slot) const {
  return config_.genesisTime + static_cast<int64_t>(slot * config_.slotDuration);
}

int64_t Ouroboros::getSlotEndTime(uint64_t slot) const {
  return getSlotStartTime(slot) + static_cast<int64_t>(config_.slotDuration);
}

Ouroboros::Roe<uint64_t> Ouroboros::getSlotLeader(uint64_t slot) const {
  if (cache_.mStakeholders.empty()) {
    return Error(1, "No stakeholders registered");
  }

  uint64_t epoch = getEpochFromSlot(slot);
  uint64_t leader = selectSlotLeader(slot, epoch);

  return leader;
}

uint64_t Ouroboros::getEpochFromSlot(uint64_t slot) const {
  if (config_.slotsPerEpoch == 0) {
    log().error << "Slots per epoch is 0";
  }
  return slot / config_.slotsPerEpoch;
}

uint64_t Ouroboros::getStake(uint64_t stakeholderId) const {
  auto it = cache_.mStakeholders.find(stakeholderId);
  if (it == cache_.mStakeholders.end()) {
    return 0;
  }
  return it->second;
}

uint64_t Ouroboros::getTotalStake() const {
  return std::accumulate(
      cache_.mStakeholders.begin(), cache_.mStakeholders.end(), uint64_t(0),
      [](uint64_t sum, const auto &pair) { return sum + pair.second; });
}

size_t Ouroboros::getStakeholderCount() const { return cache_.mStakeholders.size(); }

std::vector<Stakeholder> Ouroboros::getStakeholders() const {
  std::vector<Stakeholder> result;
  result.reserve(cache_.mStakeholders.size());

  for (const auto &[id, stake] : cache_.mStakeholders) {
    result.emplace_back();
    result.back().id = id;
    result.back().stake = stake;
  }

  return result;
}

void Ouroboros::init(const Config& config) {
  config_ = config;
  cache_ = {};
}

void Ouroboros::setStakeholders(const std::vector<Stakeholder>& stakeholders) {
  setStakeholders(stakeholders, getCurrentEpoch());
}

void Ouroboros::setStakeholders(const std::vector<Stakeholder>& stakeholders,
                                uint64_t forEpoch) {
  cache_.mStakeholders.clear();
  for (const auto& stakeholder : stakeholders) {
    cache_.mStakeholders[stakeholder.id] = stakeholder.stake;
  }
  cache_.lastStakeUpdateEpoch = forEpoch;
}

std::vector<uint64_t> Ouroboros::getEligibleLeaderPool() const {
  if (cache_.mStakeholders.empty()) {
    return {};
  }
  std::vector<std::pair<uint64_t, uint64_t>> byStake(cache_.mStakeholders.begin(),
                                                     cache_.mStakeholders.end());
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

uint64_t Ouroboros::selectSlotLeader(uint64_t slot, uint64_t epoch) const {
  std::vector<uint64_t> pool = getEligibleLeaderPool();
  if (pool.empty()) {
    return 0;
  }

  // Cryptographic hash (SHA-256) for unpredictable, verifiable leader selection
  std::string slotHash = hashSlotAndEpoch(slot, epoch);
  if (slotHash.size() < 16) {
    return pool[0];
  }

  // Use first 64 bits of hash; equal weight over eligible pool (normalized layer)
  uint64_t hashValue = std::strtoull(slotHash.substr(0, 16).c_str(), nullptr, 16);
  size_t index = static_cast<size_t>(hashValue % pool.size());
  return pool[index];
}

std::string Ouroboros::hashSlotAndEpoch(uint64_t slot, uint64_t epoch) const {
  // Domain-separated input for protocol versioning and cross-system uniqueness
  std::stringstream ss;
  ss << "pp-ledger/ouroboros/v1:slot:" << slot << ":epoch:" << epoch;
  std::string input = ss.str();
  return pp::utl::sha256(input);
}

bool Ouroboros::validateSlotLeader(uint64_t slotLeader,
                                   uint64_t slot) const {
  uint64_t epoch = getEpochFromSlot(slot);
  uint64_t expectedLeader = selectSlotLeader(slot, epoch);
  return slotLeader == expectedLeader;
}

bool Ouroboros::validateBlockTiming(int64_t blockTimestamp, uint64_t slot) const {
  int64_t slotStart = getSlotStartTime(slot);
  int64_t slotEnd = slotStart + static_cast<int64_t>(config_.slotDuration);

  return blockTimestamp >= slotStart && blockTimestamp < slotEnd;
}

} // namespace consensus
} // namespace pp
