#ifndef PP_LEDGER_BEACON_SERVER_H
#define PP_LEDGER_BEACON_SERVER_H

#include "Beacon.h"
#include "Server.h"
#include "../client/Client.h"
#include "../network/FetchServer.h"
#include "../network/TcpConnection.h"
#include "../network/Types.hpp"
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

class BeaconServer : public Server {
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

  Roe<Beacon::InitKeyConfig> init(const std::string& workDir);
  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

protected:
  bool useSignatureFile() const override { return false; }
  const char* getFileSignature() const override { return FILE_SIGNATURE; }
  const char* getFileLog() const override { return FILE_LOG; }
  const char* getServerName() const override { return "BeaconServer"; }
  int32_t getRunErrorCode() const override { return E_BEACON; }

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

    nlohmann::json ltsToJson();
    Roe<void> ltsFromJson(const nlohmann::json& jd);
  };

  struct RunFileConfig {
    std::string host{ Client::DEFAULT_HOST };
    uint16_t port{ Client::DEFAULT_BEACON_PORT };
    std::vector<std::string> whitelist; // Whitelisted beacon addresses
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
    std::vector<std::string> whitelist;
  };

  struct Config {
    NetworkConfig network;
    Beacon::CheckpointConfig checkpoint;
  };

  Roe<void> initFromWorkDir(const Beacon::InitConfig& config);
  void initHandlers();

  void registerServer(const std::string &serverAddress);
  void processQueuedRequest(QueuedRequest& qr);
  std::string binaryResponseOk(const std::string& payload) const;
  std::string binaryResponseError(uint16_t errorCode, const std::string& message) const;
  Client::BeaconState buildStateResponse() const;

  std::string handleRequest(const std::string &request);
  Roe<std::string> handleRequest(const Client::Request &request);

  Roe<std::string> hBlockGet(const Client::Request &request);
  Roe<std::string> hBlockAdd(const Client::Request &request);
  Roe<std::string> hAccountGet(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hRegister(const Client::Request &request);
  Roe<std::string> hUnsupported(const Client::Request &request);

  std::string workDir_;
  Config config_;
  Beacon beacon_;
  network::FetchServer fetchServer_;
  Client client_;

  ThreadSafeQueue<QueuedRequest> requestQueue_;
  using Handler = std::function<Roe<std::string>(const Client::Request &request)>;
  std::map<uint32_t, Handler> requestHandlers_;

  std::map<std::string, int64_t> activeServers_;
};

} // namespace pp

#endif // PP_LEDGER_BEACON_SERVER_H
