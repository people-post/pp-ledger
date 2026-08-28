#include "FetchServerConfig.h"

#include <algorithm>
#include <thread>

namespace pp {
namespace network {

SecurityConfig SecurityConfig::publicDefaults() {
  return SecurityConfig{};
}

SecurityConfig SecurityConfig::trustedDefaults() {
  SecurityConfig cfg;
  cfg.maxPayloadBytes = LedgerFrameCodec::MAX_PAYLOAD_SIZE;
  cfg.maxConcurrentConnections = 16384;
  cfg.maxConcurrentConnectionsPerIp = 256;
  cfg.maxConnectionsPerIpPerMinute = 0;
  cfg.maxRpcPerIpPerMinute = 0;
  return cfg;
}

} // namespace network
} // namespace pp
