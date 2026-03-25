#pragma once

#include <queue>
#include <mutex>

namespace pp {

/**
 * ThreadSafeQueue - A thread-safe wrapper around std::queue
 * 
 * Provides synchronized access to queue operations for multi-threaded scenarios.
 * All public methods are thread-safe.
 * 
 * @tparam T The type of elements stored in the queue
 */
template <typename T>
class ThreadSafeQueue {
public:
  /**
   * Constructor
   */
  ThreadSafeQueue() = default;

  /**
   * Destructor
   */
  ~ThreadSafeQueue() = default;

  // Delete copy operations for safety
  ThreadSafeQueue(const ThreadSafeQueue&) = delete;
  ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

  /**
   * Get the current size of the queue
   * @return Number of elements in the queue
   */
  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  /**
   * Push an element to the back of the queue
   * @param value The value to push
   */
  void push(const T& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(value);
  }

  /**
   * Push an element to the back of the queue (move version)
   * @param value The value to push
   */
  void push(T&& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(value));
  }

  /**
   * Poll an element from the front of the queue
   * @param t Reference to store the popped element
   * @return true if an element was popped, false if queue was empty
   */
  bool poll(T& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    t = std::move(queue_.front());
    queue_.pop();
    return true;
  }

private:
  mutable std::mutex mutex_;
  std::queue<T> queue_;
};

} // namespace pp
