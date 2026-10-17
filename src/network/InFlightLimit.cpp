#include "InFlightLimit.h"

namespace pp::network {

bool InFlightLimit::run(const std::string &peer, Waiter waiter, const Class cls) {
  Slot &slot = slots_[peer];
  auto &queue = cls == Class::Duty ? slot.duties : slot.normal;
  // Behind waiters of its own class (order), and a normal one behind waiting duties too.
  const bool ahead = !queue.empty() || (cls == Class::Normal && !slot.duties.empty());
  if (!ahead && hasRoom(slot.inFlight, cls)) {
    ++slot.inFlight;
    waiter.start(); // may end at once and release: no reference into slots_ is held across it
    return true;
  }
  if (cls == Class::Normal && slot.normal.size() >= maxWaiting_) {
    if (slot.inFlight == 0 && slot.duties.empty() && slot.normal.empty()) {
      slots_.erase(peer);
    }
    return false;
  }
  queue.push_back(std::move(waiter));
  return true;
}

void InFlightLimit::release(const std::string &peer) {
  auto it = slots_.find(peer);
  if (it == slots_.end()) {
    return;
  }
  if (it->second.inFlight > 0) {
    --it->second.inFlight;
  }
  startWaiting(peer);
}

void InFlightLimit::startWaiting(const std::string &peer) {
  for (;;) {
    auto it = slots_.find(peer); // a started request may release (and erase the slot) at once
    if (it == slots_.end()) {
      return;
    }
    Slot &slot = it->second;
    std::deque<Waiter> *queue = nullptr;
    if (!slot.duties.empty() && hasRoom(slot.inFlight, Class::Duty)) {
      queue = &slot.duties;
    } else if (slot.duties.empty() && !slot.normal.empty() && hasRoom(slot.inFlight, Class::Normal)) {
      queue = &slot.normal;
    }
    if (!queue) {
      if (slot.inFlight == 0 && slot.duties.empty() && slot.normal.empty()) {
        slots_.erase(it);
      }
      return;
    }
    Waiter next = std::move(queue->front());
    queue->pop_front();
    if (!next.live || !next.live()) {
      continue;
    }
    ++slot.inFlight;
    next.start(); // may release: `slot` is not used after this
    return;
  }
}

size_t InFlightLimit::inFlight(const std::string &peer) const {
  auto it = slots_.find(peer);
  return it == slots_.end() ? 0 : it->second.inFlight;
}

size_t InFlightLimit::waiting(const std::string &peer) const {
  auto it = slots_.find(peer);
  return it == slots_.end() ? 0 : it->second.duties.size() + it->second.normal.size();
}

} // namespace pp::network
