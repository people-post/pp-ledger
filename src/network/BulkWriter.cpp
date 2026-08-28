#include "BulkWriter.h"
#include "platform/NetworkPlatform.h"
#include "platform/PollWait.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_set>

#if defined(__linux__)
#include <sys/epoll.h>
#endif

namespace pp {
namespace network {

namespace {

int calculateTimeout(int timeoutMs, int defaultTimeout) {
  return (timeoutMs >= 0) ? timeoutMs : defaultTimeout;
}

} // namespace

BulkWriter::~BulkWriter() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(__linux__)
  if (epollFd_ >= 0) {
    socketClose(epollFd_);
    epollFd_ = kInvalidSocket;
  }
#endif
}

BulkWriter::Roe<void> BulkWriter::add(int fd, const void* data, size_t size) {
  if (fd < 0) {
    return Error("Invalid fd");
  }
  if (!socketSetNonBlocking(fd)) {
    return Error("Set non-blocking failed: " +
                 socketErrorString(socketLastError()));
  }
  if (!socketSetNoSigpipe(fd)) {
    return Error("Set SO_NOSIGPIPE failed: " +
                 socketErrorString(socketLastError()));
  }

  WriteJob job;
  job.fd = fd;
  job.buffer.assign(static_cast<const uint8_t*>(data),
                    static_cast<const uint8_t*>(data) + size);
  job.offset = 0;

  const int timeoutMs = calculateJobTimeout(size);
  job.expireTime =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  std::lock_guard<std::mutex> lock(mutex_);

  jobs_.push_back(std::move(job));

#if defined(__linux__)
  if (epollFd_ < 0) {
    epollFd_ = epoll_create1(0);
    if (epollFd_ < 0) {
      jobs_.pop_back();
      return Error("epoll_create1 failed: " + socketErrorString(socketLastError()));
    }
  }
  epoll_event ev {};
  ev.events = EPOLLOUT;
  ev.data.fd = fd;
  if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    jobs_.pop_back();
    return Error("epoll_ctl ADD failed: " + socketErrorString(socketLastError()));
  }
#endif
  return {};
}

BulkWriter::Roe<void> BulkWriter::add(int fd, const std::string& data) {
  return add(fd, data.data(), data.size());
}

void BulkWriter::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(__linux__)
  if (epollFd_ >= 0) {
    for (const auto& job : jobs_) {
      epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
    }
  }
#endif
  jobs_.clear();
}

void BulkWriter::runLoop() {
  const int pollMs = 100;
  while (!isStopSet()) {
    bool isEmpty;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      isEmpty = jobs_.empty();
      if (!isEmpty) {
#if defined(__linux__)
        runEpoll(pollMs);
#else
        runPoll(pollMs);
#endif
      }
    }
    if (isEmpty) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

#if defined(__linux__)
size_t BulkWriter::runEpoll(int timeoutMs) {
  const int defaultTimeout = 1000;

  while (!jobs_.empty()) {
    const int wait = calculateTimeout(timeoutMs, defaultTimeout);

    const size_t maxEvents = jobs_.size();
    std::vector<epoll_event> events(maxEvents);
    const int n = epoll_wait(epollFd_, events.data(), static_cast<int>(maxEvents), wait);

    if (n < 0) {
      if (socketInterrupted(socketLastError())) {
        continue;
      }
      break;
    }
    if (n == 0) {
      processJobs({});
      break;
    }

    std::unordered_set<int> ready;
    for (int i = 0; i < n; ++i) {
      ready.insert(events[i].data.fd);
    }

    processJobs(ready);
    break;
  }

  return jobs_.size();
}
#endif

#if !defined(__linux__)
size_t BulkWriter::runPoll(int timeoutMs) {
  const int defaultTimeout = 1000;
  if (jobs_.empty()) {
    return 0;
  }

  const int wait = calculateTimeout(timeoutMs, defaultTimeout);

  std::vector<PollWaitEntry> entries;
  entries.reserve(jobs_.size());
  for (const auto& job : jobs_) {
    PollWaitEntry entry {};
    entry.fd = job.fd;
    entry.events = POLLOUT;
    entries.push_back(entry);
  }

  const int r = pollWait(entries, wait);
  if (r < 0) {
    if (socketInterrupted(socketLastError())) {
      return jobs_.size();
    }
    return jobs_.size();
  }
  if (r == 0) {
    processJobs({});
    return jobs_.size();
  }

  std::unordered_set<int> ready;
  for (const auto& entry : entries) {
    if (entry.revents & (POLLOUT | POLLERR | POLLHUP)) {
      ready.insert(entry.fd);
    }
  }
  processJobs(ready);
  return jobs_.size();
}
#endif

void BulkWriter::unregisterFd(int fd) {
#if defined(__linux__)
  if (epollFd_ >= 0) {
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
  }
#else
  (void)fd;
#endif
}

void BulkWriter::processJobs(const std::unordered_set<int>& ready) {
  std::vector<WriteJob> next;
  next.reserve(jobs_.size());

  for (auto& job : jobs_) {
    if (isJobTimedOut(job)) {
      unregisterFd(job.fd);
      if (config_.errorCallback) {
        config_.errorCallback(job.fd, Error("Send timeout exceeded"));
      }
      socketClose(job.fd);
      continue;
    }

    if (ready.find(job.fd) == ready.end()) {
      next.push_back(std::move(job));
      continue;
    }

    const WriteResult result = attemptWrite(job);
    handleWriteResult(job, result, next);
  }

  jobs_ = std::move(next);
}

BulkWriter::WriteResult BulkWriter::attemptWrite(WriteJob& job) {
  const size_t remaining = job.buffer.size() - job.offset;
  const void* ptr = job.buffer.data() + job.offset;
#if defined(_WIN32)
  const int sent =
      ::send(static_cast<SOCKET>(job.fd), reinterpret_cast<const char*>(ptr),
             static_cast<int>(remaining), 0);
#elif defined(__linux__)
  const ssize_t sent = ::send(job.fd, ptr, remaining, MSG_NOSIGNAL);
#else
  const ssize_t sent = ::send(job.fd, ptr, remaining, 0);
#endif

  if (sent < 0) {
    if (socketWouldBlock(socketLastError())) {
      return WriteResult::Retry;
    }
    return WriteResult::Error;
  }

  job.offset += static_cast<size_t>(sent);
  return (job.offset >= job.buffer.size()) ? WriteResult::Complete : WriteResult::Retry;
}

void BulkWriter::handleWriteResult(WriteJob& job, WriteResult result,
                                   std::vector<WriteJob>& next) {
  switch (result) {
    case WriteResult::Complete:
      unregisterFd(job.fd);
      socketClose(job.fd);
      break;
    case WriteResult::Retry:
      next.push_back(std::move(job));
      break;
    case WriteResult::Error:
      unregisterFd(job.fd);
      if (config_.errorCallback) {
        config_.errorCallback(
            job.fd, Error("Send failed: " + socketErrorString(socketLastError())));
      }
      socketClose(job.fd);
      break;
  }
}

int BulkWriter::calculateJobTimeout(size_t bufferSize) const {
  const double sizeMb = static_cast<double>(bufferSize) / (1024.0 * 1024.0);
  return config_.timeout.msBase + static_cast<int>(sizeMb * config_.timeout.msPerMb);
}

bool BulkWriter::isJobTimedOut(const WriteJob& job) const {
  return std::chrono::steady_clock::now() > job.expireTime;
}

} // namespace network
} // namespace pp
