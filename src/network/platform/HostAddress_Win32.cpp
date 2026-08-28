#include "HostAddress.h"

#include <iphlpapi.h>
#include <vector>

namespace pp {
namespace network {

std::string getFirstNonLoopbackIpv4() {
  ULONG bufferSize = 15000;
  std::vector<unsigned char> buffer(bufferSize);
  IP_ADAPTER_ADDRESSES* adapters =
      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

  ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                    nullptr, adapters, &bufferSize);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(bufferSize);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                  nullptr, adapters, &bufferSize);
  }
  if (result != NO_ERROR) {
    return {};
  }

  for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr;
       adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) {
      continue;
    }
    for (IP_ADAPTER_UNICAST_ADDRESS* address = adapter->FirstUnicastAddress;
         address != nullptr; address = address->Next) {
      if (address->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      const auto* sin =
          reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
      char addrStr[INET_ADDRSTRLEN] = {};
      if (InetNtopA(AF_INET, &sin->sin_addr, addrStr, sizeof(addrStr)) == nullptr) {
        continue;
      }
      if (sin->sin_addr.S_un.S_addr == htonl(INADDR_LOOPBACK)) {
        continue;
      }
      return addrStr;
    }
  }

  return {};
}

std::string resolveAdvertisedHost(const std::string& configuredHost, SocketHandle socketFd) {
  if (!configuredHost.empty() && configuredHost != "0.0.0.0") {
    return configuredHost;
  }
  if (socketFd < 0) {
    return configuredHost.empty() ? "0.0.0.0" : configuredHost;
  }

  sockaddr_in addr {};
  int addrLen = sizeof(addr);
  if (getsockname(static_cast<SOCKET>(socketFd),
                  reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
    return "0.0.0.0";
  }

  if (addr.sin_addr.S_un.S_addr != INADDR_ANY && addr.sin_addr.S_un.S_addr != 0) {
    char addrStr[INET_ADDRSTRLEN] = {};
    if (InetNtopA(AF_INET, &addr.sin_addr, addrStr, sizeof(addrStr)) != nullptr) {
      return addrStr;
    }
  }

  const std::string discovered = getFirstNonLoopbackIpv4();
  return discovered.empty() ? "0.0.0.0" : discovered;
}

} // namespace network
} // namespace pp
