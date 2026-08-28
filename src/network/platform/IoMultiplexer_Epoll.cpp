#include "IoMultiplexer.h"

#include <sys/epoll.h>

namespace pp {
namespace network {

namespace {

class EpollMultiplexer final : public IoMultiplexer {
public:
  EpollMultiplexer() {
    epollFd_ = epoll_create1(0);
  }

  ~EpollMultiplexer() override {
    if (epollFd_ >= 0) {
      ::close(epollFd_);
      epollFd_ = kInvalidSocket;
    }
  }

  bool add(SocketHandle fd, unsigned events, bool edgeTriggered) override {
    if (epollFd_ < 0) {
      return false;
    }
    epoll_event ev {};
    if (events & Readable) {
      ev.events |= EPOLLIN;
    }
    if (events & Writable) {
      ev.events |= EPOLLOUT;
    }
    if (edgeTriggered) {
      ev.events |= EPOLLET;
    }
    ev.data.fd = fd;
    return epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == 0;
  }

  bool remove(SocketHandle fd) override {
    if (epollFd_ < 0) {
      return false;
    }
    return epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
  }

  int wait(int timeoutMs, std::vector<ReadyEvent>& ready) override {
    if (epollFd_ < 0) {
      return -1;
    }

    ready.clear();
    int total = 0;
    while (true) {
      epoll_event events[64];
      const int n = epoll_wait(epollFd_, events, 64, total == 0 ? timeoutMs : 0);
      if (n <= 0) {
        return total == 0 ? n : total;
      }
      total += n;
      for (int i = 0; i < n; ++i) {
        ReadyEvent ev {};
        ev.fd = events[i].data.fd;
        if (events[i].events & EPOLLIN) {
          ev.events |= Readable;
        }
        if (events[i].events & EPOLLOUT) {
          ev.events |= Writable;
        }
        ready.push_back(ev);
      }
      if (n < 64) {
        break;
      }
    }
    return total;
  }

private:
  SocketHandle epollFd_{kInvalidSocket};
};

} // namespace

std::unique_ptr<IoMultiplexer> IoMultiplexer::create() {
  auto mux = std::make_unique<EpollMultiplexer>();
  return mux;
}

} // namespace network
} // namespace pp
