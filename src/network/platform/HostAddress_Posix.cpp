#include "HostAddress.h"

#include <ifaddrs.h>
#include <cstring>

namespace pp {
namespace network {

std::string getFirstNonLoopbackIpv4() {
  ifaddrs* ifaddrsPtr = nullptr;
  if (getifaddrs(&ifaddrsPtr) != 0) {
    return {};
  }

  std::string result;
  for (ifaddrs* ifa = ifaddrsPtr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK) || sin->sin_addr.s_addr == 0) {
      continue;
    }
    char addrStr[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &sin->sin_addr, addrStr, sizeof(addrStr)) != nullptr) {
      result = addrStr;
      break;
    }
  }

  freeifaddrs(ifaddrsPtr);
  return result;
}

std::string resolveAdvertisedHost(const std::string& configuredHost, SocketHandle socketFd) {
  if (!configuredHost.empty() && configuredHost != "0.0.0.0") {
    return configuredHost;
  }
  if (socketFd < 0) {
    return configuredHost.empty() ? "0.0.0.0" : configuredHost;
  }

  sockaddr_in addr {};
  socklen_t addrLen = sizeof(addr);
  if (getsockname(socketFd, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
    return "0.0.0.0";
  }

  if (addr.sin_addr.s_addr != INADDR_ANY && addr.sin_addr.s_addr != 0) {
    char addrStr[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &addr.sin_addr, addrStr, sizeof(addrStr)) != nullptr) {
      return addrStr;
    }
  }

  const std::string discovered = getFirstNonLoopbackIpv4();
  return discovered.empty() ? "0.0.0.0" : discovered;
}

} // namespace network
} // namespace pp
