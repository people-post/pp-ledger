#include "ConnectionGuard.h"

namespace pp {
namespace network {

namespace {

constexpr auto kRateWindow = std::chrono::minutes(1);

} // namespace

const char* toString(ConnectionRejectReason reason) {
  switch (reason) {
    case ConnectionRejectReason::None:
      return "none";
    case ConnectionRejectReason::GlobalCap:
      return "global_cap";
    case ConnectionRejectReason::PerIpCap:
      return "per_ip_cap";
    case ConnectionRejectReason::ConnectionRate:
      return "connection_rate";
    case ConnectionRejectReason::RpcRate:
      return "rpc_rate";
  }
  return "unknown";
}

ConnectionGuard::ConnectionGuard(SecurityConfig config) : config_(std::move(config)) {}

ConnectionGuard::IpWindow& ConnectionGuard::ipState(const std::string& ip,
                                                    std::chrono::steady_clock::time_point now) {
  if (byIp_.size() >= config_.maxActiveConnectionsTracked && byIp_.find(ip) == byIp_.end()) {
    byIp_.erase(byIp_.begin());
  }
  auto& state = byIp_[ip];
  rollWindow(state, now);
  return state;
}

void ConnectionGuard::rollWindow(IpWindow& state,
                                 std::chrono::steady_clock::time_point now) {
  if (state.windowStart.time_since_epoch().count() == 0) {
    state.windowStart = now;
    return;
  }
  if (now - state.windowStart >= kRateWindow) {
    state.connectionsThisMinute = 0;
    state.rpcThisMinute = 0;
    state.windowStart = now;
  }
}

bool ConnectionGuard::tryAcceptConnection(const std::string& ip,
                                            ConnectionRejectReason& reason) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  if (globalActive_ >= config_.maxConcurrentConnections) {
    reason = ConnectionRejectReason::GlobalCap;
    return false;
  }

  auto& state = ipState(ip, now);
  if (state.activeConnections >= config_.maxConcurrentConnectionsPerIp) {
    reason = ConnectionRejectReason::PerIpCap;
    return false;
  }

  if (config_.maxConnectionsPerIpPerMinute > 0 &&
      state.connectionsThisMinute >= config_.maxConnectionsPerIpPerMinute) {
    reason = ConnectionRejectReason::ConnectionRate;
    return false;
  }

  ++globalActive_;
  ++state.activeConnections;
  ++state.connectionsThisMinute;
  reason = ConnectionRejectReason::None;
  return true;
}

void ConnectionGuard::releaseConnection(const std::string& ip) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (globalActive_ > 0) {
    --globalActive_;
  }
  auto it = byIp_.find(ip);
  if (it == byIp_.end()) {
    return;
  }
  if (it->second.activeConnections > 0) {
    --it->second.activeConnections;
  }
}

bool ConnectionGuard::tryRecordRpc(const std::string& ip) {
  if (config_.maxRpcPerIpPerMinute == 0) {
    return true;
  }

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = ipState(ip, now);
  if (state.rpcThisMinute >= config_.maxRpcPerIpPerMinute) {
    return false;
  }
  ++state.rpcThisMinute;
  return true;
}

void ConnectionGuard::expireStale(std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = byIp_.begin(); it != byIp_.end();) {
    rollWindow(it->second, now);
    if (it->second.activeConnections == 0 &&
        now - it->second.windowStart >= kRateWindow * 2) {
      it = byIp_.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace network
} // namespace pp
