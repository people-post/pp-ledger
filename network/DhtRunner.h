#pragma once

#include "Types.hpp"
#include "Module.h"
#include "ResultOrError.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pp {
namespace network {

/**
 * DhtRunner - Runs the jech/dht (BitTorrent DHT) in a background thread.
 *
 * Creates IPv4 and IPv6 UDP sockets, initializes the DHT, bootstraps from
 * configured endpoints, and runs dht_periodic in a loop. Announces our TCP
 * port under a fixed network id and collects discovered peers from
 * DHT_EVENT_VALUES / DHT_EVENT_VALUES6 into a thread-safe list.
 *
 * Integrated with BeaconServer (bootstrap peer), RelayServer (bootstrap from
 * beacon), and MinerServer (bootstrap from beacons/relays).
 */
class DhtRunner : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T>
  using Roe = ResultOrError<T, Error>;

  /** 20-byte DHT node id and info-hash (network id). */
  using NodeId = std::array<unsigned char, 20>;

  /** Default network id for pp-ledger DHT (shared by beacon, relay, miner). */
  static NodeId getDefaultNetworkId();

  struct Config {
    /** Bootstrap endpoints "host:port" (DHT UDP port of bootstrap nodes). */
    std::vector<std::string> bootstrapEndpoints;
    /** Local DHT UDP port; 0 = let OS choose. */
    uint16_t dhtPort{0};
    /** Our TCP port to announce in the DHT (e.g. beacon or miner listen port). */
    uint16_t myTcpPort{0};
    /** Network info hash (20 bytes) used for get_peers / announce. */
    NodeId networkId{};
    /** Optional path to load/save 20-byte node id for stability. */
    std::string nodeIdPath;
    /** Optional DHT version string (4 bytes); empty = NULL. */
    std::string dhtVersion;
  };

  DhtRunner();
  ~DhtRunner() override;

  DhtRunner(const DhtRunner&) = delete;
  DhtRunner& operator=(const DhtRunner&) = delete;

  /** Start DHT (create sockets, init, bootstrap, start thread and search). */
  Roe<void> start(const Config& config);
  /** Request stop and wait for thread. */
  void stop();

  /** Copy of currently discovered peers (thread-safe). */
  std::vector<IpEndpoint> getDiscoveredPeers() const;

  /** Whether the runner is currently active (thread running). */
  bool isRunning() const { return running_; }

private:
  void runLoop();
  Roe<void> createSockets(uint16_t port, int& fd4, int& fd6);
  Roe<void> loadOrCreateNodeId(const std::string& path, NodeId& out);
  void doBootstrap(const std::vector<std::string>& endpoints);
  void onDhtEvent(int event, const unsigned char* info_hash,
                  const void* data, size_t data_len);

  static void dhtCallback(void* closure, int event,
                           const unsigned char* info_hash,
                           const void* data, size_t data_len);

  Config config_;
  NodeId nodeId_{};
  int socket4_{-1};
  int socket6_{-1};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::thread thread_;
  mutable std::mutex peersMutex_;
  std::vector<IpEndpoint> discoveredPeers_;
};

} // namespace network
} // namespace pp
