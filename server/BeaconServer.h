#ifndef PP_LEDGER_BEACON_SERVER_H
#define PP_LEDGER_BEACON_SERVER_H

#include "../network/FetchServer.h"
#include "../network/Types.hpp"
#include "../lib/ResultOrError.hpp"
#include "../lib/Module.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pp {

class BeaconServer : public Module {
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
   * Stop the beacon server
   */
  void stop();

  /**
   * Check if server is running
   */
  bool isRunning() const;

  /**
   * Get list of active server addresses
   */
  std::vector<std::string> getActiveServers() const;

  /**
   * Get count of active servers
   */
  size_t getActiveServerCount() const;

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

  network::FetchServer fetchServer_;
  Config config_;

  // Track active servers (host:port -> last seen timestamp)
  mutable std::mutex serversMutex_;
  std::map<std::string, int64_t> activeServers_;
  
  // List of other beacon server addresses from config
  std::vector<std::string> otherBeaconAddresses_;
};

} // namespace pp

#endif // PP_LEDGER_BEACON_SERVER_H
