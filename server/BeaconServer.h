#ifndef PP_LEDGER_BEACON_SERVER_H
#define PP_LEDGER_BEACON_SERVER_H

#include "Beacon.h"
#include "../client/Client.h"
#include "../network/FetchServer.h"
#include "../network/TcpConnection.h"
#include "../network/Types.hpp"
#include "../lib/Service.h"
#include "../lib/ResultOrError.hpp"
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

  // Error codes
  static constexpr const int32_t E_CONFIG = -1;
  static constexpr const int32_t E_NETWORK = -2;
  static constexpr const int32_t E_BEACON = -3;
  static constexpr const int32_t E_REQUEST = -4;

  BeaconServer();
  ~BeaconServer() = default;

  Roe<void> init(const std::string& workDir);
  Service::Roe<void> run(const std::string &workDir);

protected:
  /**
   * Service thread main loop - processes queued requests
   */
  void runLoop() override;

  /**
   * Called before service thread starts - initializes beacon and network
   */
  Service::Roe<void> onStart() override;

  /**
   * Called after service thread stops - cleans up resources
   */
  void onStop() override;

private:
  constexpr static const char* FILE_INIT_CONFIG = "init-config.json";
  constexpr static const char* FILE_CONFIG = "config.json";
  constexpr static const char* FILE_LOG = "beacon.log";
  constexpr static const char* FILE_SIGNATURE = ".signature";
  constexpr static const char* DIR_DATA = "data";

  // Default configuration values
  constexpr static const uint64_t DEFAULT_SLOT_DURATION = 7; // 7 seconds per slot
  constexpr static const uint64_t DEFAULT_SLOTS_PER_EPOCH = 24 * 3600; // 7 days per epoch
  constexpr static const uint64_t DEFAULT_MAX_PENDING_TRANSACTIONS = 10000;
  constexpr static const uint64_t DEFAULT_MAX_TRANSACTIONS_PER_BLOCK = 100;
  constexpr static const uint64_t DEFAULT_MIN_FEE_PER_TRANSACTION = 1;

  // Checkpoint configuration values
  constexpr static const uint64_t DEFAULT_CHECKPOINT_SIZE = 1024ULL * 1024 * 1024; // 1GB
  constexpr static const uint64_t DEFAULT_CHECKPOINT_AGE = 365 * 24 * 3600; // 1 year

  struct InitFileConfig {
    uint64_t slotDuration{ DEFAULT_SLOT_DURATION };
    uint64_t slotsPerEpoch{ DEFAULT_SLOTS_PER_EPOCH };
    uint64_t maxPendingTransactions{ DEFAULT_MAX_PENDING_TRANSACTIONS };
    uint64_t maxTransactionsPerBlock{ DEFAULT_MAX_TRANSACTIONS_PER_BLOCK };
    uint64_t minFeePerTransaction{ DEFAULT_MIN_FEE_PER_TRANSACTION };
    std::vector<std::string> keys{ "<replace-with-public-key>" };

    nlohmann::json ltsToJson();
    Roe<void> ltsFromJson(const nlohmann::json& jd);
  };

  struct RunFileConfig {
    std::string host{ Client::DEFAULT_HOST };
    uint16_t port{ Client::DEFAULT_BEACON_PORT };
    std::vector<std::string> beacons;
    std::vector<std::string> keys{ "<replace-with-private-key>" };
    uint64_t checkpointSize{ DEFAULT_CHECKPOINT_SIZE };
    uint64_t checkpointAge{ DEFAULT_CHECKPOINT_AGE };

    nlohmann::json ltsToJson();
    Roe<void> ltsFromJson(const nlohmann::json& jd);
  };

  struct QueuedRequest {
    int fd{ -1 };
    std::string request;
  };

  struct NetworkConfig {
    network::TcpEndpoint endpoint;
    std::vector<std::string> beacons;
  };

  struct Config {
    NetworkConfig network;
    Beacon::CheckpointConfig checkpoint;
  };

  Roe<void> initFromWorkDir(const Beacon::InitConfig& config);

  void registerServer(const std::string &serverAddress);
  void connectToOtherBeacons();
  void processQueuedRequest(QueuedRequest& qr);
  std::string binaryResponseOk(const std::string& payload) const;
  std::string binaryResponseError(uint16_t errorCode, const std::string& message) const;
  nlohmann::json buildStateResponse() const;

  std::string handleRequest(const std::string &request);
  Roe<std::string> handleRequest(const Client::Request &request);

  Roe<std::string> handleBlockGetRequest(const Client::Request &request);
  Roe<std::string> handleBlockAddRequest(const Client::Request &request);
  Roe<std::string> handleJsonRequest(const std::string &payload);
  Roe<std::string> handleJsonRequest(const nlohmann::json &reqJson);

  Roe<std::string> handleRegisterRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleHeartbeatRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleQueryRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleCheckpointRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleStakeholderRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleConsensusRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleStateRequest(const nlohmann::json& reqJson);

  // Configuration
  std::string workDir_;

  // Core beacon instance
  Beacon beacon_;
  
  network::FetchServer fetchServer_;
  Config config_;

  // Request processing
  ThreadSafeQueue<QueuedRequest> requestQueue_;

  // Track active servers (host:port -> last seen timestamp)
  std::map<std::string, int64_t> activeServers_;
  
  // List of other beacon server addresses from config
  std::vector<std::string> otherBeaconAddresses_;
};

} // namespace pp

#endif // PP_LEDGER_BEACON_SERVER_H
