#include "IoMultiplexer.h"

#include <sys/event.h>
#include <sys/time.h>

namespace pp {
namespace network {

namespace {

class KqueueMultiplexer final : public IoMultiplexer {
public:
  KqueueMultiplexer() {
    kqueueFd_ = kqueue();
  }

  ~KqueueMultiplexer() override {
    if (kqueueFd_ >= 0) {
      ::close(kqueueFd_);
      kqueueFd_ = kInvalidSocket;
    }
  }

  bool add(SocketHandle fd, unsigned events, bool /*edgeTriggered*/) override {
    if (kqueueFd_ < 0) {
      return false;
    }
    if (events & Readable) {
      kevent ev {};
      EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
      if (kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        return false;
      }
    }
    if (events & Writable) {
      kevent ev {};
      EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
      if (kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        return false;
      }
    }
    return true;
  }

  bool remove(SocketHandle fd) override {
    if (kqueueFd_ < 0) {
      return false;
    }
    kevent ev {};
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr);
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr);
    return true;
  }

  int wait(int timeoutMs, std::vector<ReadyEvent>& ready) override {
    if (kqueueFd_ < 0) {
      return -1;
    }
    kevent events[32];
    timespec timeout {};
    timespec* timeoutPtr = nullptr;
    if (timeoutMs >= 0) {
      timeout.tv_sec = timeoutMs / 1000;
      timeout.tv_nsec = static_cast<long>((timeoutMs % 1000) * 1000000L);
      timeoutPtr = &timeout;
    }
    const int n = kevent(kqueueFd_, nullptr, 0, events, 32, timeoutPtr);
    if (n <= 0) {
      return n;
    }
    ready.clear();
    ready.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      ReadyEvent ev {};
      ev.fd = static_cast<SocketHandle>(events[i].ident);
      if (events[i].filter == EVFILT_READ) {
        ev.events |= Readable;
      }
      if (events[i].filter == EVFILT_WRITE) {
        ev.events |= Writable;
      }
      ready.push_back(ev);
    }
    return n;
  }

private:
  SocketHandle kqueueFd_{kInvalidSocket};
};

} // namespace

std::unique_ptr<IoMultiplexer> IoMultiplexer::create() {
  return std::make_unique<KqueueMultiplexer>();
}

} // namespace network
} // namespace pp
