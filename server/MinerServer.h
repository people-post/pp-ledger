#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "Miner.h"
#include "../network/FetchServer.h"
#include "../network/Types.hpp"
#include "../lib/Service.h"
#include "../lib/ResultOrError.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pp {

class MinerServer : public Service {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Error codes
  static constexpr const int32_t E_CONFIG = -1;
  static constexpr const int32_t E_NETWORK = -2;
  static constexpr const int32_t E_MINER = -3;

  MinerServer();
  ~MinerServer();

  /**
   * Start the miner server
   * @param dataDir Work directory containing config.json
   * @return true if server started successfully
   */
  Service::Roe<void> start(const std::string &workDir);
  
  /**
   * Get reference to underlying Miner
   */
  Miner& getMiner() { return miner_; }
  const Miner& getMiner() const { return miner_; }

protected:
  /**
   * Service thread main loop - handles block production and validation
   */
  void run() override;

  /**
   * Called before service thread starts - initializes miner and network
   */
  Service::Roe<void> onStart() override;

  /**
   * Called after service thread stops - cleans up resources
   */
  void onStop() override;

private:
  constexpr static const char* FILE_CONFIG = "config.json";
  constexpr static const char* FILE_LOG = "miner.log";
  constexpr static const char* FILE_SIGNATURE = ".signature";
  constexpr static const char* DIR_DATA = "data";

  struct NetworkConfig {
    network::TcpEndpoint endpoint;
    std::vector<std::string> beacons;
  };

  struct Config {
    NetworkConfig network;
    uint64_t minerId{ 0 };
  };

  /**
   * Load configuration from file
   * @param configPath Path to config file
   * @return ResultOrError indicating success or failure
   */
  Roe<void> loadConfig(const std::string &configPath);

  /**
   * Handle incoming request
   * @param request Request string
   * @return Response string
   */
  std::string handleRequest(const std::string &request);
  
  /**
   * Handle transaction-related requests
   */
  std::string handleTransactionRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle block-related requests
   */
  std::string handleBlockRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle mining-related requests
   */
  std::string handleMiningRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle checkpoint-related requests
   */
  std::string handleCheckpointRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle consensus-related requests
   */
  std::string handleConsensusRequest(const nlohmann::json& reqJson);
  
  /**
   * Handle status requests
   */
  std::string handleStatusRequest(const nlohmann::json& reqJson);

  /**
   * Handle block production when acting as slot leader
   */
  void handleSlotLeaderRole();

  /**
   * Handle validation when not slot leader
   */
  void handleValidatorRole();
  
  /**
   * Connect to beacon server and fetch initial state
   * @return ResultOrError indicating success or failure
   */
  Roe<void> connectToBeacon();

  /**
   * Sync blocks from beacon one at a time until local chain is up to latest block id.
   * Fetches each missing block from the beacon and adds it to the Miner.
   * @return ResultOrError indicating success or failure
   */
  Roe<void> syncBlocksFromBeacon();

  // Configuration
  std::string workDir_;

  // Core miner instance
  Miner miner_;
  
  network::FetchServer fetchServer_;
  Config config_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
