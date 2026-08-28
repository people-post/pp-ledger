#pragma once

#include "NetworkPlatform.h"

#include <string>

namespace pp {
namespace network {

// Returns the first non-loopback IPv4 address, or empty if none found.
std::string getFirstNonLoopbackIpv4();

// Resolves the host string advertised for a listening socket.
std::string resolveAdvertisedHost(const std::string& configuredHost, SocketHandle socketFd);

} // namespace network
} // namespace pp
