#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace pp {
namespace network {

struct TcpEndpoint {
  std::string address;
  uint16_t port{0};
};

inline std::ostream &operator<<(std::ostream &os, const TcpEndpoint &endpoint) {
  return os << endpoint.address << ":" << endpoint.port;
}

} // namespace network
} // namespace pp
