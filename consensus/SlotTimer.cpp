#include "SlotTimer.h"
#include <chrono>

namespace pp {
namespace consensus {

SlotTimer::SlotTimer(uint64_t slotDuration)
    : Module("consensus.slot_timer"), slotDuration_(slotDuration) {

  log().info << "Slot timer initialized with duration: " +
                    std::to_string(slotDuration_) + "s";
}

uint64_t SlotTimer::getCurrentSlot(int64_t genesisTime) const {
  int64_t currentTime = getCurrentTime();

  if (currentTime < genesisTime) {
    return 0;
  }

  int64_t elapsed = currentTime - genesisTime;
  return static_cast<uint64_t>(elapsed / slotDuration_);
}

int64_t SlotTimer::getSlotStartTime(uint64_t slot, int64_t genesisTime) const {
  return genesisTime + static_cast<int64_t>(slot * slotDuration_);
}

int64_t SlotTimer::getSlotEndTime(uint64_t slot, int64_t genesisTime) const {
  return getSlotStartTime(slot, genesisTime) +
         static_cast<int64_t>(slotDuration_);
}

bool SlotTimer::isTimeInSlot(int64_t timestamp, uint64_t slot,
                             int64_t genesisTime) const {
  int64_t slotStart = getSlotStartTime(slot, genesisTime);
  int64_t slotEnd = getSlotEndTime(slot, genesisTime);

  return timestamp >= slotStart && timestamp < slotEnd;
}

int64_t SlotTimer::getTimeUntilNextSlot(int64_t genesisTime) const {
  int64_t currentTime = getCurrentTime();
  uint64_t currentSlot = getCurrentSlot(genesisTime);
  uint64_t nextSlot = currentSlot + 1;

  int64_t nextSlotStart = getSlotStartTime(nextSlot, genesisTime);
  return nextSlotStart - currentTime;
}

int64_t SlotTimer::getTimeUntilSlot(uint64_t slot, int64_t genesisTime) const {
  int64_t currentTime = getCurrentTime();
  int64_t slotStart = getSlotStartTime(slot, genesisTime);

  return slotStart - currentTime;
}

int64_t SlotTimer::getCurrentTime() const {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(
             now.time_since_epoch())
      .count();
}

void SlotTimer::setSlotDuration(uint64_t duration) {
  slotDuration_ = duration;
  log().info << "Slot duration updated to " + std::to_string(duration) + "s";
}

} // namespace consensus
} // namespace pp
