#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "Miner.h"
#include "Server.h"
#include "../client/Client.h"
#include "../network/Types.hpp"
#include "../lib/ResultOrError.hpp"
#include <chrono>
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
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "MinerServer"; }
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

  struct RunFileConfig {
    uint64_t minerId{ 0 };
    std::vector<std::string> keys;  // Key files containing hex-encoded private keys
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
    std::vector<std::string> privateKeys;  // hex-encoded
    NetworkConfig network;
    std::map<uint64_t, Client::MinerInfo> mMiners;  // Other miners
  };

  std::string findTxSubmitAddress(uint64_t slotLeaderId);

  /** Refetch miner list from beacon. Updates config_.mMiners and lastMinerListFetchTime_. */
  void refreshMinerListFromBeacon();
  /** Periodically sync to latest block from beacon since last sync. */
  void syncBlocksPeriodically();
  Roe<Client::BeaconState> connectToBeacon();
  Roe<void> syncBlocksFromBeacon();
  /** Compute time offset in ms to beacon (beacon_time_ms = local_time_ms + offset). Call after connectToBeacon(); client_ must be set to beacon. */
  Roe<int64_t> calibrateTimeToBeacon();
  void initHandlers();
  void handleSlotLeaderRole();
  void handleValidatorRole();
  /** Retry forwarding cached transactions when slot has changed. */
  void retryCachedTransactionForwards();
  Roe<void> broadcastBlock(const Ledger::ChainNode& block);

  std::string handleParsedRequest(const Client::Request &request) override;

  Roe<std::string> hBlockGet(const Client::Request &request);
  Roe<std::string> hBlockAdd(const Client::Request &request);
  Roe<std::string> hAccountGet(const Client::Request &request);
  Roe<std::string> hTxGetByWallet(const Client::Request &request);
  Roe<std::string> hTxAdd(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hCalibration(const Client::Request &request);
  Roe<std::string> hUnsupported(const Client::Request &request);

  Miner miner_;
  Client client_;
  Config config_;

  static constexpr std::chrono::seconds MINER_LIST_REFETCH_INTERVAL{10};
  static constexpr std::chrono::seconds BLOCK_SYNC_INTERVAL{5};
  /** RTT above this (ms) triggers multiple calibration samples. */
  static constexpr int64_t RTT_THRESHOLD_MS = 200;
  /** Max number of timestamp samples when RTT is high. */
  static constexpr int CALIBRATION_SAMPLES = 5;
  /** Cached time offset to beacon in ms (beacon_time_ms = local_time_ms + offset). Set by calibrateTimeToBeacon; 0 if no beacon or calibration skipped. */
  int64_t timeOffsetToBeaconMs_{0};

  std::chrono::steady_clock::time_point lastMinerListFetchTime_{};
  std::chrono::steady_clock::time_point lastBlockSyncTime_{};
  uint64_t lastForwardRetrySlot_{0};

  using Handler = std::function<Roe<std::string>(const Client::Request &request)>;
  std::map<uint32_t, Handler> requestHandlers_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
