#pragma once

#include "NetworkPlatform.h"

#include <functional>
#include <memory>
#include <vector>

namespace pp {
namespace network {

class IoMultiplexer {
public:
  enum Event : unsigned {
    Readable = 1u,
    Writable = 2u,
  };

  struct ReadyEvent {
    SocketHandle fd{kInvalidSocket};
    unsigned events{0};
  };

  static std::unique_ptr<IoMultiplexer> create();

  IoMultiplexer() = default;
  virtual ~IoMultiplexer() = default;

  IoMultiplexer(const IoMultiplexer&) = delete;
  IoMultiplexer& operator=(const IoMultiplexer&) = delete;

  virtual bool add(SocketHandle fd, unsigned events, bool edgeTriggered = false) = 0;
  virtual bool remove(SocketHandle fd) = 0;
  virtual int wait(int timeoutMs, std::vector<ReadyEvent>& ready) = 0;
};

} // namespace network
} // namespace pp
