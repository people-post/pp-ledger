#ifndef PP_LEDGER_BEACON_SERVER_H
#define PP_LEDGER_BEACON_SERVER_H

#include "Beacon.h"
#include "../network/FetchServer.h"
#include "../network/TcpConnection.h"
#include "../network/Types.hpp"
#include "../lib/ResultOrError.hpp"
#include "../lib/Service.h"
#include "../lib/ThreadSafeQueue.hpp"
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>

namespace pp {

class BeaconServer : public Service {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  BeaconServer();
  ~BeaconServer() = default;

  /**
   * Start the beacon server
   * @param dataDir Work directory containing config.json
   * @return true if server started successfully
   */
  bool start(const std::string &dataDir);

  /**
   * Get list of active server addresses
   */
  std::vector<std::string> getActiveServers() const;

  /**
   * Get count of active servers
   */
  size_t getActiveServerCount() const;
  
  /**
   * Get reference to underlying Beacon
   */
  Beacon& getBeacon() { return beacon_; }
  const Beacon& getBeacon() const { return beacon_; }

protected:
  /**
   * Service thread main loop - processes queued requests
   */
  void run() override;

  /**
   * Called before service thread starts - initializes beacon and network
   */
  bool onStart() override;

  /**
   * Called after service thread stops - cleans up resources
   */
  void onStop() override;

private:
  struct NetworkConfig {
    network::TcpEndpoint endpoint;
    std::vector<std::string> beacons;
  };

  struct Config {
    NetworkConfig network;
  };

  /**
   * Load configuration from file
   * @param configPath Path to config file
   * @return ResultOrError indicating success or failure
   */
  Roe<void> loadConfig(const std::string &configPath);

  /**
   * Handle incoming request from Server
   * @param request Request string from Server
   * @return Response string
   */
  std::string handleServerRequest(const std::string &request);

  /**
   * Register a server as active
   * @param serverAddress Server address in host:port format
   */
  void registerServer(const std::string &serverAddress);

  /**
   * Connect to other beacon servers from config
   */
  void connectToOtherBeacons();
  
  /**
   * Handle block-related requests
   */
  std::string handleBlockRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle checkpoint-related requests
   */
  std::string handleCheckpointRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle stakeholder-related requests
   */
  std::string handleStakeholderRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle consensus-related requests
   */
  std::string handleConsensusRequest(const nlohmann::json& reqJson);

  struct QueuedRequest {
    std::string request;
    std::shared_ptr<network::TcpConnection> connection;
  };

  // Configuration
  std::string dataDir_;

  // Core beacon instance
  Beacon beacon_;
  
  network::FetchServer fetchServer_;
  Config config_;

  // Request processing
  ThreadSafeQueue<QueuedRequest> requestQueue_;

  // Track active servers (host:port -> last seen timestamp)
  mutable std::mutex serversMutex_;
  std::map<std::string, int64_t> activeServers_;
  
  // List of other beacon server addresses from config
  std::vector<std::string> otherBeaconAddresses_;
};

} // namespace pp

#endif // PP_LEDGER_BEACON_SERVER_H
