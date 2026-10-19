#include "ledger/server/RequestQueue.h"

#include <algorithm>

namespace pp {

RequestQueue::RequestQueue(size_t capacity) : capacity_(capacity), lowCapacity_(std::max<size_t>(1, capacity / 4)) {}

bool RequestQueue::push(std::string peerId, std::string body, Reply reply, Lane lane) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
      return false;
    }
    const size_t size = body.size();
    Item item{std::move(peerId), std::move(body), std::move(reply), Clock::now(), {}, lane};
    switch (lane) {
    case Lane::Trusted:
      if (trusted_.size() >= kTrustedCapacity || trustedBytes_ + size > kMaxQueuedBytes) {
        return false;
      }
      trustedBytes_ += size;
      trusted_.push_back(std::move(item));
      break;
    case Lane::Normal:
      if (normal_.size() >= capacity_ || bytes_ + size > kMaxQueuedBytes) {
        return false;
      }
      bytes_ += size;
      normal_.push_back(std::move(item));
      break;
    case Lane::Low: {
      if (low_.size() >= lowCapacity_ || bytes_ + size > kMaxQueuedBytes) {
        return false;
      }
      auto &queuedByPeer = lowByPeer_[item.peerId];
      if (queuedByPeer >= kLowPerPeer) {
        return false;
      }
      ++queuedByPeer;
      bytes_ += size;
      low_.push_back(std::move(item));
      break;
    }
    }
  }
  cv_.notify_one();
  return true;
}

bool RequestQueue::pushTask(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
      return false;
    }
    // Not capacity-limited: a completion carries a reply some client waits on.
    normal_.push_back(Item{{}, {}, {}, Clock::now(), std::move(task), Lane::Normal});
  }
  cv_.notify_one();
  return true;
}

std::optional<RequestQueue::Item> RequestQueue::popUntil(Clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mu_);
  const bool ready = cv_.wait_until(
      lock, deadline, [this]() { return !trusted_.empty() || !normal_.empty() || !low_.empty() || closed_; });
  if (!ready || (trusted_.empty() && normal_.empty() && low_.empty())) {
    return std::nullopt;
  }
  const bool othersWait = !normal_.empty() || !low_.empty();
  if (!trusted_.empty() && (!othersWait || trustedStreak_ < kTrustedPerOther)) {
    ++trustedStreak_;
    Item item = std::move(trusted_.front());
    trusted_.pop_front();
    taken(item);
    return item;
  }
  trustedStreak_ = 0;
  const bool takeLow = !low_.empty() && (normal_.empty() || normalStreak_ >= kNormalPerLow);
  if (!takeLow) {
    ++normalStreak_;
    Item item = std::move(normal_.front());
    normal_.pop_front();
    taken(item);
    return item;
  }
  normalStreak_ = 0;
  Item item = std::move(low_.front());
  low_.pop_front();
  taken(item);
  if (auto it = lowByPeer_.find(item.peerId); it != lowByPeer_.end() && --it->second == 0) {
    lowByPeer_.erase(it);
  }
  return item;
}

std::optional<RequestQueue::Item> RequestQueue::popTaskUntil(Clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mu_);
  auto firstTask = [this]() {
    return std::find_if(normal_.begin(), normal_.end(), [](const Item &item) { return bool(item.task); });
  };
  if (!cv_.wait_until(lock, deadline, [&]() { return firstTask() != normal_.end() || closed_; })) {
    return std::nullopt;
  }
  auto it = firstTask();
  if (it == normal_.end()) {
    return std::nullopt;
  }
  Item item = std::move(*it);
  normal_.erase(it);
  return item;
}

std::deque<RequestQueue::Item> RequestQueue::close() {
  std::deque<Item> pending;
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    pending.swap(normal_);
    for (auto &item : low_) {
      pending.push_back(std::move(item));
    }
    for (auto &item : trusted_) {
      pending.push_back(std::move(item));
    }
    low_.clear();
    trusted_.clear();
    lowByPeer_.clear();
    bytes_ = 0;
    trustedBytes_ = 0;
  }
  cv_.notify_all();
  return pending;
}

void RequestQueue::taken(const Item &item) {
  size_t &bytes = item.lane == Lane::Trusted ? trustedBytes_ : bytes_;
  bytes -= std::min(bytes, item.body.size());
}

bool RequestQueue::isClosed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return closed_;
}

size_t RequestQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return trusted_.size() + normal_.size() + low_.size();
}

} // namespace pp
