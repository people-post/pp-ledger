#include "PollWait.h"

#include <poll.h>

namespace pp {
namespace network {

int pollWait(std::vector<PollWaitEntry>& entries, int timeoutMs) {
  if (entries.empty()) {
    return 0;
  }

  std::vector<pollfd> pfds;
  pfds.reserve(entries.size());
  for (const auto& entry : entries) {
    pollfd pfd {};
    pfd.fd = entry.fd;
    pfd.events = entry.events;
    pfds.push_back(pfd);
  }

  const int r = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeoutMs);
  if (r < 0) {
    return -1;
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].revents = pfds[i].revents;
  }
  return r;
}

} // namespace network
} // namespace pp
