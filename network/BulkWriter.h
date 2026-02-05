#pragma once

#include "ResultOrError.hpp"
#include "Service.h"

#include <cstddef>
#include <functional>
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

  BulkWriter() = default;
  ~BulkWriter() override;

  // Add a socket fd and the single payload to write. Takes ownership of a copy
  // of the data. The fd is set to non-blocking. Caller must not close the fd
  // until the write is done (BulkWriter closes it when write completes).
  Roe<void> add(int fd, const void *data, size_t size);

  Roe<void> add(int fd, const std::string &data);

  // Optional: called when a fd fails (e.g. send error). fd is not closed by
  // BulkWriter in that case; caller may close it.
  void setErrorCallback(ErrorCallback cb) { errorCallback_ = std::move(cb); }

  // Run the write loop until all fds have written their data and been closed,
  // or until timeoutMs elapses (0 = no timeout). Returns number of fds still
  // pending (0 = all done). Use when not running as a service.
  size_t runToCompletion(int timeoutMs = 0);

  // Perform one poll + write pass. Returns number of fds still pending.
  // Useful when driving from an external event loop.
  size_t runOnce(int timeoutMs = 0);

  // Number of fds still pending (not yet fully written / closed).
  size_t pendingCount() const { return jobs_.size(); }

  // Remove all pending jobs without writing; does not close fds.
  void clear();

protected:
  void runLoop() override;

private:
  struct WriteJob {
    int fd{-1};
    std::vector<uint8_t> buffer;
    size_t offset{0};
  };

  size_t runInternal(int timeoutMs, bool once);

  std::vector<WriteJob> jobs_;
  ErrorCallback errorCallback_;
#if defined(__linux__)
  int epollFd_{-1};
#endif
};

} // namespace network
} // namespace pp
