#ifndef PP_LEDGER_HEALTH_ENDPOINT_H
#define PP_LEDGER_HEALTH_ENDPOINT_H

#include "common/Module.h"
#include "common/ResultOrError.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace pp {

/**
 * What a node reports to probes and Prometheus (docs/ops/CONFIGURATION.md § Health and metrics).
 * Filled on the server thread (Server::fillHealth), read on the endpoint's: a plain copy.
 */
struct HealthSnapshot {
  std::string role;      ///< beacon | relay | miner
  std::string peerId;    ///< this node's network identity
  std::string networkId; ///< the chain's
  bool ready{false};
  std::string notReady;  ///< why not, when !ready
  uint64_t nextBlockId{0};
  std::optional<uint64_t> upstreamNextBlockId; ///< relay, miner: the upstream's, last heard
  std::optional<int64_t> lastSyncAgeSeconds;   ///< relay, miner: since the last successful sync
  std::optional<uint64_t> signedTipHeight;
  uint64_t generation{0};      ///< beacon generation in force
  bool halted{false};          ///< equivocation evidence held (beacon-authority ba6)
  bool recoveryPending{false}; ///< a recovery waits (ba7)
  bool standingDown{false};    ///< beacon: another key is certified
  uint64_t requestQueueDepth{0};
};

/**
 * Opt-in HTTP probe and metrics endpoint, on its own thread:
 *  - `GET /healthz` 200 while the server thread publishes (it is not stuck), else 503;
 *  - `GET /readyz` 200 when the node is ready (synced, not halted…), else 503 with the reason;
 *  - `GET /metrics` Prometheus text.
 * Reads only the last published snapshot: probes never touch the chain.
 */
class HealthEndpoint : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  /** A server thread silent this long is reported unhealthy. */
  static constexpr std::chrono::seconds kStaleAfter{30};

  HealthEndpoint();
  ~HealthEndpoint() override;

  /** Listen on `bind`:`port` (port 0: a free one, see port()). */
  Roe<void> start(const std::string &bind, uint16_t port);
  void stop();
  uint16_t port() const { return port_; }

  /** From the server thread. */
  void publish(HealthSnapshot snapshot);

  /** Rendered bodies (also for tests). */
  static std::string renderMetrics(const HealthSnapshot &snapshot, bool fresh);

private:
  /** The last snapshot and whether it is recent enough to trust. */
  std::pair<HealthSnapshot, bool> current() const;

  mutable std::mutex mu_;
  HealthSnapshot snapshot_;
  std::chrono::steady_clock::time_point publishedAt_{};
  bool published_{false};
  std::unique_ptr<httplib::Server> http_;
  std::thread thread_;
  uint16_t port_{0};
};

} // namespace pp

#endif // PP_LEDGER_HEALTH_ENDPOINT_H
