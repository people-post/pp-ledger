#include "IoMultiplexer.h"
#include "PollWait.h"

namespace pp {
namespace network {

namespace {

class WsapollMultiplexer final : public IoMultiplexer {
public:
  bool add(SocketHandle fd, unsigned events, bool /*edgeTriggered*/) override {
    for (auto& tracked : tracked_) {
      if (tracked.fd == fd) {
        tracked.events = events;
        return true;
      }
    }
    Tracked tracked {};
    tracked.fd = fd;
    tracked.events = events;
    tracked_.push_back(tracked);
    return true;
  }

  bool remove(SocketHandle fd) override {
    for (auto it = tracked_.begin(); it != tracked_.end(); ++it) {
      if (it->fd == fd) {
        tracked_.erase(it);
        return true;
      }
    }
    return false;
  }

  int wait(int timeoutMs, std::vector<ReadyEvent>& ready) override {
    if (tracked_.empty()) {
      return 0;
    }
    std::vector<PollWaitEntry> entries;
    entries.reserve(tracked_.size());
    for (const auto& tracked : tracked_) {
      PollWaitEntry entry {};
      entry.fd = tracked.fd;
      if (tracked.events & Readable) {
        entry.events |= POLLRDNORM;
      }
      if (tracked.events & Writable) {
        entry.events |= POLLWRNORM;
      }
      entries.push_back(entry);
    }

    const int n = pollWait(entries, timeoutMs);
    if (n <= 0) {
      return n;
    }

    ready.clear();
    for (const auto& entry : entries) {
      if (entry.revents == 0) {
        continue;
      }
      ReadyEvent ev {};
      ev.fd = entry.fd;
      if (entry.revents & (POLLRDNORM | POLLIN)) {
        ev.events |= Readable;
      }
      if (entry.revents & (POLLWRNORM | POLLOUT)) {
        ev.events |= Writable;
      }
      if (entry.revents & (POLLERR | POLLHUP)) {
        ev.events |= Readable;
        ev.events |= Writable;
      }
      ready.push_back(ev);
    }
    return static_cast<int>(ready.size());
  }

private:
  struct Tracked {
    SocketHandle fd{kInvalidSocket};
    unsigned events{0};
  };

  std::vector<Tracked> tracked_;
};

} // namespace

std::unique_ptr<IoMultiplexer> IoMultiplexer::create() {
  return std::make_unique<WsapollMultiplexer>();
}

} // namespace network
} // namespace pp
