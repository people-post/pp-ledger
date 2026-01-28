#include "EpochManager.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace pp {
namespace consensus {

// ========== EpochManager Implementation ==========

EpochManager::EpochManager(uint64_t slotsPerEpoch, uint64_t slotDuration)
    : slotsPerEpoch_(slotsPerEpoch),
      slotDuration_(slotDuration), genesisTime_(0), cachedCurrentEpoch_(0),
      lastUpdateTime_(0) {
  auto now = std::chrono::system_clock::now();
  genesisTime_ =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  log().info << "Epoch manager initialized: " + std::to_string(slotsPerEpoch_) +
                    " slots per epoch, " + std::to_string(slotDuration_) +
                    "s slot duration";
}

void EpochManager::initializeEpoch(uint64_t epochNumber,
                                   const std::string &nonce) {
  EpochInfo info;
  info.number = epochNumber;
  info.nonce = nonce;
  info.startSlot = epochNumber * slotsPerEpoch_;
  info.endSlot = info.startSlot + slotsPerEpoch_ - 1;
  info.startTime = getSlotStartTime(info.startSlot);
  info.endTime = getSlotEndTime(info.endSlot);

  epochs_[epochNumber] = info;

  log().info << "Initialized epoch " + std::to_string(epochNumber) +
                    " [slots " + std::to_string(info.startSlot) + "-" +
                    std::to_string(info.endSlot) + "]";
}

void EpochManager::finalizeEpoch(uint64_t epochNumber,
                                 const std::vector<std::string> &blockHashes) {
  auto it = epochs_.find(epochNumber);
  if (it == epochs_.end()) {
    log().warning << "Cannot finalize uninitialized epoch " +
                         std::to_string(epochNumber);
    return;
  }

  log().info << "Finalized epoch " + std::to_string(epochNumber) + " with " +
                    std::to_string(blockHashes.size()) + " blocks";
}

EpochManager::EpochInfo EpochManager::getEpochInfo(uint64_t epochNumber) const {
  auto it = epochs_.find(epochNumber);
  if (it != epochs_.end()) {
    return it->second;
  }

  // Return empty info if not found
  EpochInfo info;
  info.number = epochNumber;
  info.startSlot = epochNumber * slotsPerEpoch_;
  info.endSlot = info.startSlot + slotsPerEpoch_ - 1;
  info.startTime = getSlotStartTime(info.startSlot);
  info.endTime = getSlotEndTime(info.endSlot);

  return info;
}

EpochManager::EpochInfo EpochManager::getCurrentEpochInfo() const {
  uint64_t currentEpoch = getCurrentEpoch();
  return getEpochInfo(currentEpoch);
}

uint64_t EpochManager::getCurrentEpoch() const {
  auto now = std::chrono::system_clock::now();
  int64_t currentTime =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  // Cache result for performance
  if (currentTime == lastUpdateTime_) {
    return cachedCurrentEpoch_;
  }

  uint64_t currentSlot = getCurrentSlot();
  cachedCurrentEpoch_ = getEpochFromSlot(currentSlot);
  lastUpdateTime_ = currentTime;

  return cachedCurrentEpoch_;
}

bool EpochManager::isEpochInitialized(uint64_t epochNumber) const {
  return epochs_.find(epochNumber) != epochs_.end();
}

void EpochManager::setSlotLeader(uint64_t epochNumber, uint64_t slot,
                                 const std::string &leader) {
  auto it = epochs_.find(epochNumber);
  if (it == epochs_.end()) {
    log().warning << "Cannot set slot leader for uninitialized epoch " +
                         std::to_string(epochNumber);
    return;
  }

  it->second.slotLeaders[slot] = leader;
}

std::string EpochManager::getSlotLeader(uint64_t epochNumber,
                                        uint64_t slot) const {
  auto it = epochs_.find(epochNumber);
  if (it == epochs_.end()) {
    return "";
  }

  auto leaderIt = it->second.slotLeaders.find(slot);
  if (leaderIt != it->second.slotLeaders.end()) {
    return leaderIt->second;
  }

  return "";
}

void EpochManager::setGenesisTime(int64_t timestamp) {
  genesisTime_ = timestamp;
  log().info << "Genesis time set to " + std::to_string(timestamp);
}

void EpochManager::setSlotsPerEpoch(uint64_t slots) {
  slotsPerEpoch_ = slots;
  log().info << "Slots per epoch updated to " + std::to_string(slots);
}

void EpochManager::setSlotDuration(uint64_t duration) {
  slotDuration_ = duration;
  log().info << "Slot duration updated to " + std::to_string(duration) + "s";
}

uint64_t EpochManager::getCurrentSlot() const {
  auto now = std::chrono::system_clock::now();
  int64_t currentTime =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  if (currentTime < genesisTime_) {
    return 0;
  }

  int64_t elapsed = currentTime - genesisTime_;
  return static_cast<uint64_t>(elapsed / slotDuration_);
}

uint64_t EpochManager::getEpochFromSlot(uint64_t slot) const {
  return slot / slotsPerEpoch_;
}

uint64_t EpochManager::getSlotInEpoch(uint64_t slot) const {
  return slot % slotsPerEpoch_;
}

int64_t EpochManager::getSlotStartTime(uint64_t slot) const {
  return genesisTime_ + static_cast<int64_t>(slot * slotDuration_);
}

int64_t EpochManager::getSlotEndTime(uint64_t slot) const {
  return getSlotStartTime(slot) + static_cast<int64_t>(slotDuration_);
}

} // namespace consensus
} // namespace pp
