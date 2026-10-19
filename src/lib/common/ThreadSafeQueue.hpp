#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace pp {

/**
 * ThreadSafeQueue - A thread-safe wrapper around std::queue
 */
template <typename T>
class ThreadSafeQueue {
public:
  explicit ThreadSafeQueue(size_t maxSize = 0) : maxSize_(maxSize) {}

  ThreadSafeQueue(const ThreadSafeQueue&) = delete;
  ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

  void setMaxSize(size_t maxSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxSize_ = maxSize;
  }

  size_t maxSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxSize_;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  void push(const T& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(value);
    cv_.notify_one();
  }

  void push(T&& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(value));
    cv_.notify_one();
  }

  bool tryPush(const T& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (maxSize_ > 0 && queue_.size() >= maxSize_) {
      return false;
    }
    queue_.push(value);
    cv_.notify_one();
    return true;
  }

  bool tryPush(T&& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (maxSize_ > 0 && queue_.size() >= maxSize_) {
      return false;
    }
    queue_.push(std::move(value));
    cv_.notify_one();
    return true;
  }

  bool poll(T& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    t = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  template <typename Rep, typename Period>
  bool waitPop(T& t, const std::chrono::duration<Rep, Period>& timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this]() { return !queue_.empty(); })) {
      return false;
    }
    t = std::move(queue_.front());
    queue_.pop();
    return true;
  }

private:
  size_t maxSize_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<T> queue_;
};

} // namespace pp
