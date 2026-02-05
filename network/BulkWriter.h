#pragma once

#include "ResultOrError.hpp"
#include "Service.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace pp {
namespace network {

/**
 * BulkWriter manages many socket fds in non-blocking mode.
 * Each fd has one fixed payload; after the full payload is written, the fd is closed.
 * Inherits Service: when started, runs the write loop in a dedicated thread.
 */
class BulkWriter : public Service {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  using ErrorCallback = std::function<void(int fd, const Error &)>;

  struct TimeoutConfig {
    // Timeout config for send operations;
    // if a send takes longer than msBase + (size in MB * msPerMb), it is considered a failure.
    int msBase{1000};
    int msPerMb{1000};
  };

  struct Config {
    TimeoutConfig timeout;
    ErrorCallback errorCallback{ nullptr };
  };

  BulkWriter() = default;
  ~BulkWriter() override;

  // BulkWriter in that case; caller may close it.
  void setConfig(const Config &config) { config_ = config; }

  // Add a socket fd and the single payload to write. Takes ownership of a copy
  // of the data. The fd is set to non-blocking. Caller must not close the fd
  // until the write is done (BulkWriter closes it when write completes).
  Roe<void> add(int fd, const std::string &data);

protected:
  void runLoop() override;

private:
  struct WriteJob {
    int fd{-1};
    std::vector<uint8_t> buffer;
    size_t offset{0};
    std::chrono::steady_clock::time_point expireTime;
  };

  enum class WriteResult {
    Complete,  // Write completed successfully
    Retry,     // Needs retry (EAGAIN or partial write)
    Error      // Write error occurred
  };

  Roe<void> add(int fd, const void *data, size_t size);

  // Remove all pending jobs without writing; does not close fds.
  void clear();

  // Attempt to write data for a single job
  WriteResult attemptWrite(WriteJob &job);

  // Handle the result of a write attempt
  void handleWriteResult(WriteJob &job, WriteResult result, 
                         std::vector<WriteJob> &next);

  // Process jobs using epoll (mutex must be held by caller)
  size_t runEpoll(int timeoutMs);
  void processJobs(const std::unordered_set<int> &ready);

  // Calculate timeout for a job based on its size
  int calculateJobTimeout(size_t bufferSize) const;

  // Check if a job has timed out
  bool isJobTimedOut(const WriteJob &job) const;

  std::mutex mutex_;  // Protects jobs_ and epollFd_
  std::vector<WriteJob> jobs_;
  int epollFd_{-1};
  Config config_;
};

} // namespace network
} // namespace pp
