#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "Miner.h"
#include "Server.h"
#include "../client/Client.h"
#include "../network/FetchServer.h"
#include "../network/Types.hpp"
#include "../lib/ResultOrError.hpp"
#include "../lib/ThreadSafeQueue.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pp {

class MinerServer : public Server {
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

  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

protected:
  const char* getFileSignature() const override { return FILE_SIGNATURE; }
  const char* getFileLog() const override { return FILE_LOG; }
  const char* getServerName() const override { return "MinerServer"; }
  int32_t getRunErrorCode() const override { return E_MINER; }

  /**
   * Service thread main loop - handles block production and validation
   */
  void runLoop() override;

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

  struct QueuedRequest {
    int fd{ -1 };
    std::string request;
  };

  struct RunFileConfig {
    uint64_t minerId{ 0 };
    std::string key{ "key.txt" };  // Key file containing hex-encoded private key (default: key.txt)
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
    std::string privateKey;  // hex-encoded
    NetworkConfig network;
  };

  std::string getSlotLeaderAddress() const;
  Roe<Client::BeaconState> connectToBeacon();
  Roe<void> syncBlocksFromBeacon();
  void handleSlotLeaderRole();
  void handleValidatorRole();
  Roe<void> broadcastBlock(const Ledger::ChainNode& block);
  void processQueuedRequest(QueuedRequest& qr);

  std::string binaryResponseOk(const std::string& payload) const;
  std::string binaryResponseError(uint16_t errorCode, const std::string& message) const;

  std::string handleRequest(const std::string &request);
  Roe<std::string> handleRequest(const Client::Request &request);

  Roe<std::string> handleBlockGetRequest(const Client::Request &request);
  Roe<std::string> handleBlockAddRequest(const Client::Request &request);
  Roe<std::string> handleAccountGetRequest(const Client::Request &request);
  Roe<std::string> handleTransactionAddRequest(const Client::Request &request);
  Roe<std::string> handleStatusRequest(const Client::Request &request);

  Miner miner_;
  network::FetchServer fetchServer_;
  Client client_;
  Config config_;

  // Request processing
  ThreadSafeQueue<QueuedRequest> requestQueue_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
