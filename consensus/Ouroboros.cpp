#include "Ouroboros.h"
#include <algorithm>
#include <chrono>
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

uint64_t Ouroboros::getCurrentSlot() const {
  auto now = std::chrono::system_clock::now();
  int64_t localTime =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  // Use beacon time for slot: beacon_time = local_time + timeOffset_
  int64_t currentTime = localTime + config_.timeOffset;

  if (currentTime < config_.genesisTime) {
    return 0;
  }

  int64_t elapsed = currentTime - config_.genesisTime;
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
  if (mStakeholders_.empty()) {
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

uint64_t Ouroboros::getTotalStake() const {
  return std::accumulate(
      mStakeholders_.begin(), mStakeholders_.end(), uint64_t(0),
      [](uint64_t sum, const auto &pair) { return sum + pair.second; });
}

size_t Ouroboros::getStakeholderCount() const { return mStakeholders_.size(); }

std::vector<Stakeholder> Ouroboros::getStakeholders() const {
  std::vector<Stakeholder> result;
  result.reserve(mStakeholders_.size());

  for (const auto &[id, stake] : mStakeholders_) {
    result.emplace_back();
    result.back().id = id;
    result.back().stake = stake;
  }

  return result;
}

void Ouroboros::init(const Config& config) {
  config_ = config;
}

void Ouroboros::registerStakeholder(uint64_t id, uint64_t stake) {
  if (stake == 0) {
    log().warning << "Cannot register stakeholder '" + std::to_string(id) + "' with zero stake";
    return;
  }

  mStakeholders_[id] = stake;
  log().info << "Registered stakeholder '" + std::to_string(id) +
                    "' with stake: " + std::to_string(stake);
}

void Ouroboros::updateStake(uint64_t id, uint64_t newStake) {
  auto it = mStakeholders_.find(id);
  if (it == mStakeholders_.end()) {
    log().warning << "Cannot update stake for unknown stakeholder: " + std::to_string(id);
    return;
  }

  uint64_t oldStake = it->second;
  it->second = newStake;
  log().info << "Updated stake for '" + std::to_string(id) + "' from " +
                    std::to_string(oldStake) + " to " +
                    std::to_string(newStake);
}

bool Ouroboros::removeStakeholder(uint64_t id) {
  auto it = mStakeholders_.find(id);
  if (it == mStakeholders_.end()) {
    log().warning << "Cannot remove unknown stakeholder: " + std::to_string(id);
    return false;
  }

  mStakeholders_.erase(it);
  log().info << "Removed stakeholder: " + std::to_string(id);
  return true;
}

uint64_t Ouroboros::selectSlotLeader(uint64_t slot, uint64_t epoch) const {
  // Simple deterministic leader selection based on stake weight
  // In production, this would use VRF (Verifiable Random Function)

  uint64_t totalStake = getTotalStake();
  if (totalStake == 0) {
    return 0;
  }

  // Create a deterministic hash from slot and epoch
  std::string slotHash = hashSlotAndEpoch(slot, epoch);

  // Convert hash to a number in range [0, totalStake)
  uint64_t hashValue = 0;
  for (size_t i = 0; i < std::min(slotHash.size(), size_t(8)); ++i) {
    hashValue = (hashValue << 8) | static_cast<uint8_t>(slotHash[i]);
  }
  uint64_t position = hashValue % totalStake;

  // Select stakeholder based on cumulative stake
  uint64_t cumulative = 0;
  for (const auto &[id, stake] : mStakeholders_) {
    cumulative += stake;
    if (position < cumulative) {
      return id;
    }
  }

  // Fallback to first stakeholder
  return mStakeholders_.begin()->first;
}

std::string Ouroboros::hashSlotAndEpoch(uint64_t slot, uint64_t epoch) const {
  // Simple hash function (in production, use cryptographic hash)
  std::stringstream ss;
  ss << "slot:" << slot << ":epoch:" << epoch;
  std::string input = ss.str();

  // Simple hash computation
  uint64_t hash = 0x123456789abcdef0ULL;
  for (char c : input) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 0x100000001b3ULL;
  }

  ss.str("");
  ss << std::hex << std::setfill('0') << std::setw(16) << hash;
  return ss.str();
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
