#include "PollWait.h"

namespace pp {
namespace network {

int pollWait(std::vector<PollWaitEntry>& entries, int timeoutMs) {
  if (entries.empty()) {
    return 0;
  }

  std::vector<WSAPOLLFD> pfds;
  pfds.reserve(entries.size());
  for (const auto& entry : entries) {
    WSAPOLLFD pfd {};
    pfd.fd = static_cast<SOCKET>(entry.fd);
    pfd.events = entry.events;
    pfds.push_back(pfd);
  }

  const int r = WSAPoll(pfds.data(), static_cast<ULONG>(pfds.size()),
                        static_cast<INT>(timeoutMs));
  if (r == SOCKET_ERROR) {
    return -1;
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].revents = static_cast<short>(pfds[i].revents);
  }
  return r;
}

} // namespace network
} // namespace pp
