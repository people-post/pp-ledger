#pragma once

#include "BulkWriter.h"
#include "LedgerFrameCodec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace pp {
namespace network {

struct SecurityConfig {
  uint32_t maxPayloadBytes{512 * 1024};
  std::chrono::milliseconds readIdleTimeout{15000};
  std::chrono::milliseconds readTotalTimeout{30000};
  size_t maxConcurrentConnections{4096};
  size_t maxConcurrentConnectionsPerIp{64};
  size_t maxConnectionsPerIpPerMinute{60};
  size_t maxRpcPerIpPerMinute{120};
  size_t maxActiveConnectionsTracked{10000};

  static SecurityConfig publicDefaults();
  static SecurityConfig trustedDefaults();
};

struct PerformanceConfig {
  size_t handlerWorkers{4};
  size_t maxRequestQueueSize{4096};
  int listenBacklog{1024};
  size_t writeFastPathMaxBytes{4096};

  BulkWriter::TimeoutConfig bulkWriterTimeout{};
};

} // namespace network
} // namespace pp
