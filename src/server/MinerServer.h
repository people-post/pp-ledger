#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "Miner.h"
#include "NetworkAnchor.h"
#include "BlockSync.h"
#include "UpstreamTipWatch.h"
#include "BroadcastTally.h"
#include "Server.h"
#include "../client/Client.h"
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
  };

  /**
   * Leader, at the start of `slot`: pull pending transactions from upstream
   * (TX_PULL) into the pool. True once they are in, or after txPullWait() so
   * the slot is not missed; false while still waiting.
   */
  bool pullTransactionsForSlot(uint64_t slot);
  std::chrono::milliseconds txPullWait() const;
  void syncBlocksPeriodically();
  /** Start a background sync (rate-limited to one per slot unless bypassed). */
  void trySyncBlocksFromBeacon(bool bypassRateLimit = false);
  /** BlockSync wiring and completion (server thread). */
  void initBlockSync();
  void onBlockSyncFinished(const BlockSync::Result &result);
  /** Re-register with the upstream every REGISTER_RENEW_INTERVAL so the beacon keeps our record. */
  void renewRegistrationPeriodically();
  /** Our registration, signed with all our keys for `networkId` (the beacon verifies it). */
  Roe<Client::MinerInfo> signedRegistration(const std::string &networkId) const;
  Roe<Client::BeaconState> connectToBeacon();
  Roe<int64_t> calibrateTimeToBeacon();
  void initHandlers();
  void handleSlotLeaderRole();
  /** Send a produced (sealed) block to every upstream in parallel. */
  void startBroadcast(const Ledger::ChainNode& block);
  void onBroadcastResult(uint64_t broadcastId, size_t upstream, Client::Roe<bool> result);
  /** First upstream accepted it: commit our seal. */
  void commitProducedBlock(const Ledger::ChainNode& block);
  Client::Roe<void> dialPeerMultiaddr(const std::string& multiaddr, const std::string& peer_key);
  Roe<void> dialUpstreamIndex(size_t index);
  Roe<void> dialActiveUpstream();
  Roe<size_t> selectBestUpstreamIndex();
  Roe<void> verifyUpstreamState(const Client::BeaconState& state);
  Roe<void> verifyGenesisAnchor();

  std::string handleParsedRequest(const Client::Request &request) override;
  bool handleDeferred(const Client::Request &request, const RequestQueue::Reply &reply) override;

  // Deferred: reply after an outbound call or the next sync
  void dBlockGet(const Client::Request &request, const RequestQueue::Reply &reply);
  void dTxAdd(const Client::Request &request, const RequestQueue::Reply &reply);
  /** After a sync: answer block gets that were waiting for it. */
  void resolvePendingBlockGets();

  Roe<std::string> hAccountGet(const Client::Request &request);
  Roe<std::string> hTxGetByWallet(const Client::Request &request);
  Roe<std::string> hTxGetByIndex(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hCalibration(const Client::Request &request);
  Roe<std::string> hUnsupported(const Client::Request &request);

  Miner miner_;
  /** Upstream (beacon) client. */
  Client client_;
  /** Block broadcast to upstreams (one dial key per upstream). */
  Client broadcastClient_;

  /** A produced block awaiting its broadcast results (at most one). */
  struct PendingBroadcast {
    uint64_t id{0};
    Ledger::ChainNode block;
    BroadcastTally tally{0};
  };
  std::optional<PendingBroadcast> broadcast_;
  uint64_t nextBroadcastId_{1};
  Config config_;

  /** Longest a leader waits for its transaction pull before producing without it. */
  static constexpr std::chrono::milliseconds TX_PULL_MAX_WAIT{1000};
  uint64_t txPulledSlot_{UINT64_MAX};
  bool txPullInFlight_{false};
  std::chrono::steady_clock::time_point txPullStarted_{};
  /** Well inside the beacon's record TTL (BeaconServer::MINER_RECORD_TTL, 5 min). */
  static constexpr std::chrono::seconds REGISTER_RENEW_INTERVAL{60};
  std::chrono::steady_clock::time_point lastRegistration_{};
  bool registrationInFlight_{false};
  /** Network id the upstream advertised at connect; registrations are signed for it. */
  std::string registrationNetworkId_;
  static constexpr int64_t SYNC_BEFORE_SLOT_SECONDS = 2;
  static constexpr int64_t RTT_THRESHOLD_MS = 200;
  static constexpr int CALIBRATION_SAMPLES = 5;
  int64_t timeOffsetToBeaconMs_{0};

  std::chrono::steady_clock::time_point lastBlockSyncTime_{};
  uint64_t lastSyncedEpoch_{0};
  size_t active_upstream_index_{0};

  using Handler = std::function<Roe<std::string>(const Client::Request &request)>;
  std::map<uint32_t, Handler> requestHandlers_;
  using DeferredHandler =
      std::function<void(const Client::Request &request, const RequestQueue::Reply &reply)>;
  std::map<uint32_t, DeferredHandler> deferredHandlers_;

  struct PendingBlockGet {
    uint64_t blockId;
    RequestQueue::Reply reply;
  };
  /** Block gets beyond our tip, answered after the next sync. */
  std::vector<PendingBlockGet> pendingBlockGets_;
  bool blockSyncRequested_{false};
  std::unique_ptr<BlockSync> blockSync_;
  /** BLOCK_WAIT on the upstream: requests a sync as soon as it has a new block. */
  std::unique_ptr<UpstreamTipWatch> tipWatch_;
  std::optional<BlockSync::Result> lastSyncResult_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
