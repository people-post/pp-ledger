#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "Miner.h"
#include "NetworkAnchor.h"
#include "Server.h"
#include "../client/Client.h"
#include "../network/Types.hpp"
#include "common/ResultOrError.hpp"
#include "lib/common/Meta.h"
#include <chrono>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

namespace pp {

class MinerServer : public Server {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  static constexpr const int32_t E_CONFIG = -1;
  static constexpr const int32_t E_NETWORK = -2;
  static constexpr const int32_t E_MINER = -3;
  static constexpr const int32_t E_REQUEST = -4;

  MinerServer();
  ~MinerServer() override;

  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

protected:
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "MinerServer"; }
  int32_t getRunErrorCode() const override { return E_MINER; }

  void runLoop() override;
  Service::Roe<void> onStart() override;
  void onStop() override;

private:
  constexpr static const char* FILE_CONFIG = "config.json";
  constexpr static const char* FILE_LOG = "miner.log";
  constexpr static const char* FILE_SIGNATURE = ".signature";
  constexpr static const char* DIR_DATA = "data";

  struct RunFileConfig {
    uint64_t minerId{ 0 };
    std::vector<std::string> keys;
    std::string host{ Client::DEFAULT_HOST };
    uint16_t port{ Client::DEFAULT_MINER_PORT };
    std::vector<std::string> beacons;
    NetworkAnchor network_anchor;

    pp::common::Object ltsToJson() const;
    Roe<void> ltsFromJson(const pp::common::Object& jd);
  };

  struct NetworkConfig {
    uint16_t udp_port{ Client::DEFAULT_MINER_PORT };
    std::vector<std::string> beacon_multiaddrs;
    NetworkAnchor network_anchor;
  };

  struct Config {
    uint64_t minerId{ 0 };
    std::vector<std::string> privateKeys;
    NetworkConfig network;
    std::map<uint64_t, Client::MinerInfo> mMiners;
  };

  std::string findTxSubmitAddress(uint64_t slotLeaderId);
  void refreshMinerListFromBeacon();
  void syncBlocksPeriodically();
  void trySyncBlocksFromBeacon(bool bypassRateLimit = false);
  Roe<Client::BeaconState> connectToBeacon();
  Roe<void> syncBlocksFromBeacon();
  Roe<int64_t> calibrateTimeToBeacon();
  void initHandlers();
  void handleSlotLeaderRole();
  void handleValidatorRole();
  void retryCachedTransactionForwards();
  Roe<void> broadcastBlock(const Ledger::ChainNode& block);
  Client::Roe<void> dialPeerMultiaddr(const std::string& multiaddr, const std::string& peer_key);
  Roe<void> dialUpstreamIndex(size_t index);
  Roe<void> dialActiveUpstream();
  Roe<size_t> selectBestUpstreamIndex();
  Roe<void> verifyUpstreamState(const Client::BeaconState& state);
  Roe<void> verifyGenesisAnchor();

  std::string handleParsedRequest(const Client::Request &request) override;

  Roe<std::string> hBlockGet(const Client::Request &request);
  Roe<std::string> hBlockAdd(const Client::Request &request);
  Roe<std::string> hAccountGet(const Client::Request &request);
  Roe<std::string> hTxGetByWallet(const Client::Request &request);
  Roe<std::string> hTxGetByIndex(const Client::Request &request);
  Roe<std::string> hTxAdd(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hCalibration(const Client::Request &request);
  Roe<std::string> hUnsupported(const Client::Request &request);

  Miner miner_;
  Client client_;
  Config config_;

  static constexpr std::chrono::seconds MINER_LIST_REFETCH_INTERVAL{10};
  static constexpr int64_t SYNC_BEFORE_SLOT_SECONDS = 2;
  static constexpr int64_t RTT_THRESHOLD_MS = 200;
  static constexpr int CALIBRATION_SAMPLES = 5;
  int64_t timeOffsetToBeaconMs_{0};

  std::chrono::steady_clock::time_point lastMinerListFetchTime_{};
  std::chrono::steady_clock::time_point lastBlockSyncTime_{};
  uint64_t lastSyncedEpoch_{0};
  uint64_t lastForwardRetrySlot_{0};
  size_t active_upstream_index_{0};

  using Handler = std::function<Roe<std::string>(const Client::Request &request)>;
  std::map<uint32_t, Handler> requestHandlers_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
