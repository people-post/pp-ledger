#ifndef PP_LEDGER_BEACON_H
#define PP_LEDGER_BEACON_H

#include "../network/FetchServer.h"
#include "../lib/ResultOrError.hpp"
#include "../lib/Service.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pp {

class Beacon : public network::FetchServer {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  Beacon();
  ~Beacon() override = default;

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

private:
  struct NetworkConfig {
    std::string host;
    uint16_t port{ 0 };
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

  Config config_;

  // Track active servers (host:port -> last seen timestamp)
  mutable std::mutex serversMutex_;
  std::map<std::string, int64_t> activeServers_;
  
  // List of other beacon server addresses from config
  std::vector<std::string> otherBeaconAddresses_;
};

} // namespace pp

#endif // PP_LEDGER_BEACON_H
