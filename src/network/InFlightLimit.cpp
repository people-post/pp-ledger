#include "InFlightLimit.h"

namespace pp::network {

void InFlightLimit::run(const std::string &peer, Waiter waiter) {
  Slot &slot = slots_[peer];
  if (slot.inFlight < limit_ && slot.waiting.empty()) {
    ++slot.inFlight;
    waiter.start(); // may end at once and release: no reference into slots_ is held across it
    return;
  }
  slot.waiting.push_back(std::move(waiter));
}

void InFlightLimit::release(const std::string &peer) {
  auto it = slots_.find(peer);
  if (it == slots_.end()) {
    return;
  }
  if (it->second.inFlight > 0) {
    --it->second.inFlight;
  }
  while (!it->second.waiting.empty()) {
    Waiter next = std::move(it->second.waiting.front());
    it->second.waiting.pop_front();
    if (!next.live || !next.live()) {
      continue;
    }
    ++it->second.inFlight;
    next.start(); // may release (and erase the slot): `it` is not used after this
    return;
  }
  if (it->second.inFlight == 0) {
    slots_.erase(it);
  }
}

size_t InFlightLimit::inFlight(const std::string &peer) const {
  auto it = slots_.find(peer);
  return it == slots_.end() ? 0 : it->second.inFlight;
}

size_t InFlightLimit::waiting(const std::string &peer) const {
  auto it = slots_.find(peer);
  return it == slots_.end() ? 0 : it->second.waiting.size();
}

} // namespace pp::network
