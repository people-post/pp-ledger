#pragma once

#include "../FetchServerConfig.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pp {
namespace network {

enum class ConnectionRejectReason {
  None,
  GlobalCap,
  PerIpCap,
  ConnectionRate,
  RpcRate,
};

class ConnectionGuard {
public:
  explicit ConnectionGuard(SecurityConfig config);

  bool tryAcceptConnection(const std::string& ip, ConnectionRejectReason& reason);
  void releaseConnection(const std::string& ip);
  bool tryRecordRpc(const std::string& ip);

  void expireStale(std::chrono::steady_clock::time_point now);

private:
  struct IpWindow {
    size_t activeConnections{0};
    size_t connectionsThisMinute{0};
    size_t rpcThisMinute{0};
    std::chrono::steady_clock::time_point windowStart{};
  };

  IpWindow& ipState(const std::string& ip, std::chrono::steady_clock::time_point now);
  void rollWindow(IpWindow& state, std::chrono::steady_clock::time_point now);

  SecurityConfig config_;
  mutable std::mutex mutex_;
  size_t globalActive_{0};
  std::unordered_map<std::string, IpWindow> byIp_;
};

const char* toString(ConnectionRejectReason reason);

} // namespace network
} // namespace pp
