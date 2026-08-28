#pragma once

#include "NetworkPlatform.h"

#include <cstddef>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#endif

namespace pp {
namespace network {

struct PollWaitEntry {
  SocketHandle fd{kInvalidSocket};
  short events{0};
  short revents{0};
};

// Returns ready count, or -1 on error.
int pollWait(std::vector<PollWaitEntry>& entries, int timeoutMs);

} // namespace network
} // namespace pp
