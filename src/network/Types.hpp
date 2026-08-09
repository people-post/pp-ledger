#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace pp {
namespace network {

struct IpEndpoint {
  std::string address;
  uint16_t port{0};

  std::string ltsToString() const {
    return address + ":" + std::to_string(port);
  }

  static IpEndpoint ltsFromString(const std::string &endpointStr) {
    IpEndpoint endpoint;
    size_t colonPos = endpointStr.find(':');
    if (colonPos != std::string::npos) {
      endpoint.address = endpointStr.substr(0, colonPos);
      endpoint.port = static_cast<uint16_t>(
          std::stoul(endpointStr.substr(colonPos + 1)));
    } else {
      endpoint.address = endpointStr;
      endpoint.port = 0;
    }
    return endpoint;
  }
};

inline std::ostream &operator<<(std::ostream &os, const IpEndpoint &endpoint) {
  return os << endpoint.address << ":" << endpoint.port;
}

} // namespace network
} // namespace pp
