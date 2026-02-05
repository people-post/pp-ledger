#include "BulkWriter.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <poll.h>
#endif

namespace pp {
namespace network {

namespace {

int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
  return 0;
}

} // namespace

BulkWriter::~BulkWriter() {
#if defined(__linux__)
  if (epollFd_ >= 0) {
    ::close(epollFd_);
    epollFd_ = -1;
  }
#endif
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
  jobs_.push_back(std::move(job));

#if defined(__linux__)
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
#endif
  return {};
}

BulkWriter::Roe<void> BulkWriter::add(int fd, const std::string &data) {
  return add(fd, data.data(), data.size());
}

void BulkWriter::clear() {
#if defined(__linux__)
  if (epollFd_ >= 0) {
    for (const auto &job : jobs_) {
      epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
    }
  }
#endif
  jobs_.clear();
}

void BulkWriter::runLoop() {
  const int pollMs = 100;
  while (!isStopSet()) {
    runOnce(pollMs);
    if (pendingCount() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

size_t BulkWriter::runToCompletion(int timeoutMs) {
  return runInternal(timeoutMs, false);
}

size_t BulkWriter::runOnce(int timeoutMs) {
  return runInternal(timeoutMs, true);
}

size_t BulkWriter::runInternal(int timeoutMs, bool once) {
  if (jobs_.empty()) return 0;

  int elapsed = 0;
  const int defaultTimeout = 1000;

#if defined(__linux__)
  // Linux: epoll path
  while (!jobs_.empty()) {
    int wait = defaultTimeout;
    if (once) {
      wait = (timeoutMs >= 0) ? timeoutMs : defaultTimeout;
    } else if (timeoutMs > 0) {
      int remaining = timeoutMs - elapsed;
      wait = (remaining <= 0) ? 0 : std::min(remaining, defaultTimeout);
    }

    size_t maxEvents = jobs_.size();
    std::vector<struct epoll_event> events(maxEvents);
    int n = epoll_wait(epollFd_, events.data(), static_cast<int>(maxEvents), wait);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      elapsed += defaultTimeout;
      if (once || (timeoutMs > 0 && elapsed >= timeoutMs)) break;
      continue;
    }

    std::unordered_set<int> ready;
    for (int i = 0; i < n; ++i) {
      ready.insert(events[i].data.fd);
    }

    std::vector<WriteJob> next;
    next.reserve(jobs_.size());
    for (auto &job : jobs_) {
      auto it = ready.find(job.fd);
      if (it == ready.end()) {
        next.push_back(std::move(job));
        continue;
      }

      size_t remaining = job.buffer.size() - job.offset;
      const void *ptr = job.buffer.data() + job.offset;
      ssize_t sent = ::send(job.fd, ptr, remaining, 0);

      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          next.push_back(std::move(job));
          continue;
        }
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
        if (errorCallback_) {
          errorCallback_(job.fd, Error("Send failed: " + std::string(std::strerror(errno))));
        }
        continue;
      }

      job.offset += static_cast<size_t>(sent);
      if (job.offset >= job.buffer.size()) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, job.fd, nullptr);
        ::close(job.fd);
      } else {
        next.push_back(std::move(job));
      }
    }
    jobs_ = std::move(next);
    if (once) break;
  }
#else
  // macOS / other: poll path
  while (!jobs_.empty()) {
    std::vector<struct pollfd> pfds;
    pfds.reserve(jobs_.size());
    for (const auto &job : jobs_) {
      struct pollfd pfd = {};
      pfd.fd = job.fd;
      pfd.events = POLLOUT;
      pfds.push_back(pfd);
    }

    int wait = defaultTimeout;
    if (once) {
      wait = (timeoutMs >= 0) ? timeoutMs : defaultTimeout;
    } else if (timeoutMs > 0) {
      int remaining = timeoutMs - elapsed;
      wait = (remaining <= 0) ? 0 : std::min(remaining, defaultTimeout);
    }

    int r = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), wait);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) {
      elapsed += defaultTimeout;
      if (once || (timeoutMs > 0 && elapsed >= timeoutMs)) break;
      continue;
    }

    std::vector<WriteJob> next;
    next.reserve(jobs_.size());
    for (size_t i = 0; i < jobs_.size(); ++i) {
      WriteJob &job = jobs_[i];
      bool writable = (i < pfds.size() && (pfds[i].revents & (POLLOUT | POLLERR | POLLHUP)));

      if (!writable) {
        next.push_back(std::move(job));
        continue;
      }

      if (pfds[i].revents & (POLLERR | POLLHUP)) {
        if (errorCallback_) {
          errorCallback_(job.fd, Error("Socket error or hangup"));
        }
        ::close(job.fd);
        continue;
      }

      size_t remaining = job.buffer.size() - job.offset;
      const void *ptr = job.buffer.data() + job.offset;
      ssize_t sent = ::send(job.fd, ptr, remaining, 0);

      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          next.push_back(std::move(job));
          continue;
        }
        if (errorCallback_) {
          errorCallback_(job.fd, Error("Send failed: " + std::string(std::strerror(errno))));
        }
        continue;
      }

      job.offset += static_cast<size_t>(sent);
      if (job.offset >= job.buffer.size()) {
        ::close(job.fd);
      } else {
        next.push_back(std::move(job));
      }
    }
    jobs_ = std::move(next);
    if (once) break;
  }
#endif

  return jobs_.size();
}

} // namespace network
} // namespace pp
