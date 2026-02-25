#ifndef PP_LEDGER_BEACON_SERVER_H
#define PP_LEDGER_BEACON_SERVER_H

#include "Beacon.h"
#include "Server.h"
#include "../client/Client.h"
#include "../network/TcpConnection.h"
#include "../network/Types.hpp"
#include "../lib/ResultOrError.hpp"
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
  ~BeaconServer() override = default;

  Roe<Beacon::InitKeyConfig> init(const std::string& workDir);
  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

protected:
  bool useSignatureFile() const override { return false; }
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "BeaconServer"; }
  int32_t getRunErrorCode() const override { return E_BEACON; }

  void customizeFetchServerConfig(network::FetchServer::Config &config) override;

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
  constexpr static const uint64_t DEFAULT_MAX_CUSTOM_META_SIZE = 1 * 1024 * 1024; // 1MB
  constexpr static const uint64_t DEFAULT_MAX_TRANSACTIONS_PER_BLOCK = 10 * 1024; // 10K
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_A = 1;  // Base fee per transaction
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_B = 1;  // Fee per custom meta KiB
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_C = 1;  // Fee per custom meta KiB^2
  constexpr static const uint64_t DEFAULT_FREE_CUSTOM_META_SIZE = 1024; // 1KiB free custom meta size

  // Checkpoint configuration values
  constexpr static const uint64_t DEFAULT_CHECKPOINT_MIN_BLOCKS = 1 << 20; // 1 million blocks
  constexpr static const uint64_t DEFAULT_CHECKPOINT_MIN_AGE_SECONDS = 365 * 24 * 3600; // 1 year (365 days)
  constexpr static const uint64_t DEFAULT_MAX_VALIDATION_TIMESPAN_SECONDS = 86400; // 24 hours

  struct InitFileConfig {
    uint64_t slotDuration{ DEFAULT_SLOT_DURATION };
    uint64_t slotsPerEpoch{ DEFAULT_SLOTS_PER_EPOCH };
    uint64_t maxCustomMetaSize{ DEFAULT_MAX_CUSTOM_META_SIZE };
    uint64_t maxTransactionsPerBlock{ DEFAULT_MAX_TRANSACTIONS_PER_BLOCK };
    std::vector<uint16_t> minFeeCoefficients{ DEFAULT_MIN_FEE_COEFF_A,
                                              DEFAULT_MIN_FEE_COEFF_B,
                                              DEFAULT_MIN_FEE_COEFF_C };
    uint64_t freeCustomMetaSize{ DEFAULT_FREE_CUSTOM_META_SIZE };
    uint64_t checkpointMinBlocks{ DEFAULT_CHECKPOINT_MIN_BLOCKS };
    uint64_t checkpointMinAgeSeconds{ DEFAULT_CHECKPOINT_MIN_AGE_SECONDS };
    uint64_t maxValidationTimespanSeconds{ DEFAULT_MAX_VALIDATION_TIMESPAN_SECONDS };

    nlohmann::json ltsToJson();
    Roe<void> ltsFromJson(const nlohmann::json& jd);
  };

  struct RunFileConfig {
    std::string host{ Client::DEFAULT_HOST };
    uint16_t port{ Client::DEFAULT_BEACON_PORT };
    std::vector<std::string> whitelist; // Whitelisted beacon addresses

    nlohmann::json ltsToJson();
    Roe<void> ltsFromJson(const nlohmann::json& jd);
  };

  struct NetworkConfig {
    network::TcpEndpoint endpoint;
    std::vector<std::string> whitelist;
  };

  struct Config {
    NetworkConfig network;
    Chain::CheckpointConfig checkpoint;
  };

  Roe<void> initFromWorkDir(const Beacon::InitConfig& config);
  void initHandlers();

  void registerServer(const Client::MinerInfo &minerInfo);
  Client::BeaconState buildStateResponse() const;

  std::string handleParsedRequest(const Client::Request &request) override;

  Roe<std::string> hBlockGet(const Client::Request &request);
  Roe<std::string> hBlockAdd(const Client::Request &request);
  Roe<std::string> hAccountGet(const Client::Request &request);
  Roe<std::string> hTxGetByWallet(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hCalibration(const Client::Request &request);
  Roe<std::string> hRegister(const Client::Request &request);
  Roe<std::string> hMinerList(const Client::Request &request);
  Roe<std::string> hUnsupported(const Client::Request &request);

  Config config_;
  Beacon beacon_;
  Client client_;

  using Handler = std::function<Roe<std::string>(const Client::Request &request)>;
  std::map<uint32_t, Handler> requestHandlers_;

  std::map<uint64_t, Client::MinerInfo> mMiners_;
};

} // namespace pp

#endif // PP_LEDGER_BEACON_SERVER_H
