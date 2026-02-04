#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "Miner.h"
#include "../client/Client.h"
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
  static constexpr const int32_t E_REQUEST = -4;

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

  struct RunFileConfig {
    uint64_t minerId{ 0 };
    uint64_t tokenId{ AccountBuffer::ID_GENESIS };
    std::string privateKey;  // hex-encoded
    std::string host{ Client::DEFAULT_HOST };
    uint16_t port{ Client::DEFAULT_MINER_PORT };
    std::vector<std::string> beacons;

    nlohmann::json ltsToJson();
    Roe<void> ltsFromJson(const nlohmann::json& jd);
  };

  struct NetworkConfig {
    network::TcpEndpoint endpoint;
    std::vector<std::string> beacons;
  };

  struct Config {
    uint64_t minerId{ 0 };
    uint64_t tokenId{ AccountBuffer::ID_GENESIS };
    std::string privateKey;  // hex-encoded
    NetworkConfig network;
  };

  std::string getSlotLeaderAddress() const;
  Roe<Client::BeaconState> connectToBeacon();
  Roe<void> syncBlocksFromBeacon();
  void handleSlotLeaderRole();
  void handleValidatorRole();
  Roe<void> broadcastBlock(const Ledger::ChainNode& block);

  std::string binaryResponseOk(const std::string& payload) const;
  std::string binaryResponseError(uint16_t errorCode, const std::string& message) const;

  std::string handleRequest(const std::string &request);
  Roe<std::string> handleRequest(const Client::Request &request);

  Roe<std::string> handleBlockGetRequest(const Client::Request &request);
  Roe<std::string> handleBlockAddRequest(const Client::Request &request);
  Roe<std::string> handleTransactionAddRequest(const Client::Request &request);
  Roe<std::string> handleJsonRequest(const std::string &payload);
  Roe<std::string> handleJsonRequest(const nlohmann::json &reqJson);

  Roe<std::string> handleCheckpointRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleConsensusRequest(const nlohmann::json& reqJson);
  Roe<std::string> handleStatusRequest(const nlohmann::json& reqJson);

  std::string workDir_;
  Miner miner_;
  network::FetchServer fetchServer_;
  Client client_;
  Config config_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
