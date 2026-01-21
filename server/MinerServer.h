#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "Miner.h"
#include "../network/FetchServer.h"
#include "../network/Types.hpp"
#include "../lib/ResultOrError.hpp"
#include "../lib/Service.h"
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

  MinerServer();
  ~MinerServer();

  /**
   * Start the miner server
   * @param dataDir Work directory containing config.json
   * @return true if server started successfully
   */
  bool start(const std::string &dataDir);
  
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
    std::string minerId;
    uint64_t stake;
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
   * Handle block production when acting as slot leader
   */
  void handleSlotLeaderRole();

  /**
   * Handle validation when not slot leader
   */
  void handleValidatorRole();

  // Configuration
  std::string dataDir_;

  // Core miner instance
  Miner miner_;
  
  network::FetchServer fetchServer_;
  Config config_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
