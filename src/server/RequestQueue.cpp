#include "RequestQueue.h"

#include <algorithm>

namespace pp {

RequestQueue::RequestQueue(size_t capacity) : capacity_(capacity) {}

bool RequestQueue::push(std::string body, Reply reply) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_ || items_.size() >= capacity_) {
      return false;
    }
    items_.push_back(Item{std::move(body), std::move(reply), Clock::now(), {}});
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
    items_.push_back(Item{{}, {}, Clock::now(), std::move(task)});
  }
  cv_.notify_one();
  return true;
}

std::optional<RequestQueue::Item> RequestQueue::popUntil(Clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!cv_.wait_until(lock, deadline, [this]() { return !items_.empty() || closed_; }) || items_.empty()) {
    return std::nullopt;
  }
  Item item = std::move(items_.front());
  items_.pop_front();
  return item;
}

std::optional<RequestQueue::Item> RequestQueue::popTaskUntil(Clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mu_);
  auto firstTask = [this]() {
    return std::find_if(items_.begin(), items_.end(), [](const Item &item) { return bool(item.task); });
  };
  if (!cv_.wait_until(lock, deadline, [&]() { return firstTask() != items_.end() || closed_; })) {
    return std::nullopt;
  }
  auto it = firstTask();
  if (it == items_.end()) {
    return std::nullopt;
  }
  Item item = std::move(*it);
  items_.erase(it);
  return item;
}

std::deque<RequestQueue::Item> RequestQueue::close() {
  std::deque<Item> pending;
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    pending.swap(items_);
  }
  cv_.notify_all();
  return pending;
}

bool RequestQueue::isClosed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return closed_;
}

size_t RequestQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return items_.size();
}

} // namespace pp
