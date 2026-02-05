#include "BulkWriter.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>

namespace pp {
namespace network {

namespace {

int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
  return 0;
}

int calculateTimeout(int timeoutMs, int defaultTimeout) {
  return (timeoutMs >= 0) ? timeoutMs : defaultTimeout;
}

} // namespace

BulkWriter::~BulkWriter() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (epollFd_ >= 0) {
    ::close(epollFd_);
    epollFd_ = -1;
  }
}

BulkWriter::Roe<void> BulkWriter::add(int fd, const void *data, size_t size) {
  if (fd < 0) {
    return Error("Invalid fd");
  }
  if (setNonBlocking(fd) < 0) {
    return Error("Set non-blocking failed: " + std::string(std::strerror(errno)));
  }

  WriteJob job;
  job.fd = fd;
  job.buffer.assign(static_cast<const uint8_t *>(data),
                   static_cast<const uint8_t *>(data) + size);
  job.offset = 0;
  
  int timeoutMs = calculateJobTimeout(size);
  job.expireTime = std::chrono::steady_clock::now() + 
                   std::chrono::milliseconds(timeoutMs);

  std::lock_guard<std::mutex> lock(mutex_);
  
  jobs_.push_back(std::move(job));

  if (epollFd_ < 0) {
    epollFd_ = epoll_create1(0);
    if (epollFd_ < 0) {
      jobs_.pop_back();
      return Error("epoll_create1 failed: " + std::string(std::strerror(errno)));
    }
  }
  struct epoll_event ev = {};
  ev.events = EPOLLOUT;
  ev.data.fd = fd;
  if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    jobs_.pop_back();
    return Error("epoll_ctl ADD failed: " + std::string(std::strerror(errno)));
  }
  return {};
}

BulkWriter::Roe<void> BulkWriter::add(int fd, const std::string &data) {
  return add(fd, data.data(), data.size());
}

void BulkWriter::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (epollFd_ >= 0) {
    for (const auto &job : jobs_) {
      epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
    }
  }
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
        runEpoll(pollMs);
      }
    }
    if (isEmpty) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

size_t BulkWriter::runEpoll(int timeoutMs) {
  const int defaultTimeout = 1000;

  while (!jobs_.empty()) {
    int wait = calculateTimeout(timeoutMs, defaultTimeout);

    size_t maxEvents = jobs_.size();
    std::vector<struct epoll_event> events(maxEvents);
    int n = epoll_wait(epollFd_, events.data(), static_cast<int>(maxEvents), wait);
    
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
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

void BulkWriter::processJobs(const std::unordered_set<int> &ready) {
  std::vector<WriteJob> next;
  next.reserve(jobs_.size());
  
  for (auto &job : jobs_) {
    // Check for timeout first
    if (isJobTimedOut(job)) {
      epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
      if (config_.errorCallback) {
        config_.errorCallback(job.fd, Error("Send timeout exceeded"));
      }
      continue;
    }

    if (ready.find(job.fd) == ready.end()) {
      next.push_back(std::move(job));
      continue;
    }

    WriteResult result = attemptWrite(job);
    handleWriteResult(job, result, next);
  }
  
  jobs_ = std::move(next);
}

BulkWriter::WriteResult BulkWriter::attemptWrite(WriteJob &job) {
  size_t remaining = job.buffer.size() - job.offset;
  const void *ptr = job.buffer.data() + job.offset;
  ssize_t sent = ::send(job.fd, ptr, remaining, 0);

  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return WriteResult::Retry;
    }
    return WriteResult::Error;
  }

  job.offset += static_cast<size_t>(sent);
  return (job.offset >= job.buffer.size()) ? WriteResult::Complete : WriteResult::Retry;
}

void BulkWriter::handleWriteResult(WriteJob &job, WriteResult result, 
                                   std::vector<WriteJob> &next) {
  switch (result) {
    case WriteResult::Complete:
      epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
      ::close(job.fd);
      break;
    case WriteResult::Retry:
      next.push_back(std::move(job));
      break;
    case WriteResult::Error:
      epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
      if (config_.errorCallback) {
        config_.errorCallback(job.fd, Error("Send failed: " + std::string(std::strerror(errno))));
      }
      break;
  }
}

int BulkWriter::calculateJobTimeout(size_t bufferSize) const {
  // Convert buffer size to MB (using floating point for precision)
  double sizeMb = static_cast<double>(bufferSize) / (1024.0 * 1024.0);
  int timeoutMs = config_.timeout.msBase + 
                  static_cast<int>(sizeMb * config_.timeout.msPerMb);
  return timeoutMs;
}

bool BulkWriter::isJobTimedOut(const WriteJob &job) const {
  return std::chrono::steady_clock::now() > job.expireTime;
}

} // namespace network
} // namespace pp
