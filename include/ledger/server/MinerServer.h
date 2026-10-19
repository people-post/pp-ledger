#ifndef PP_LEDGER_MINER_SERVER_H
#define PP_LEDGER_MINER_SERVER_H

#include "ledger/server/BroadcastTally.h"
#include "ledger/server/FollowerServer.h"
#include "ledger/server/Miner.h"
#include "ledger/server/NetworkAnchor.h"
#include "ledger/client/Client.h"
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pp {

class MinerServer : public FollowerServer {
public:
  MinerServer();

  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

protected:
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "MinerServer"; }
  void fillHealth(Client::NodeHealth &health) const override;
  Service::Roe<void> onStart() override;

  /** Renews the registration; leads its slots. */
  void roleDuties() override;
  /** The anchored genesis, when the miner starts there (chooseStartingBlock). */
  std::optional<std::string> firstBlockHash() const override;
  /** The synced starting block agrees with the replayed proof (an anchored checkpoint start). */
  pp::Roe<void> checkStartingAuthority(uint64_t blockId, const ChainAuthority &authority,
                                       const Client::Wallet &admin) const override;
  void onSynced() override { warnIfNotRegisteredMiner(); }
  /** A produced block awaits its broadcast: a synced block would be refused. */
  bool syncHeld() const override;
  /** Leading the next slot, which starts within SYNC_BEFORE_SLOT_SECONDS: sync first. */
  bool syncDue() const override;

private:
  constexpr static const char* FILE_LOG = "miner.log";
  constexpr static const char* FILE_SIGNATURE = ".signature";
  constexpr static const char* DIR_DATA = "data";

  /** config.json: the follower's fields (FollowerConfig), `minerId` and `keys` (key files). */
  struct RunFileConfig : FollowerConfig {
    uint64_t minerId{0};
    std::vector<std::string> keys;

    RunFileConfig() : FollowerConfig(Client::DEFAULT_MINER_PORT) {}
    Roe<void> fromJson(const pp::common::Object &jd);
    /** A template: what is unknown stands as a placeholder. */
    pp::common::Object toJson() const;
  };

  struct Config {
    uint64_t minerId{0};
    std::vector<std::string> privateKeys;
  };

  /**
   * Leader, at the start of `slot`: pull pending transactions from upstream
   * (TX_PULL) into the pool. True once they are in, or after txPullWait() so
   * the slot is not missed; false while still waiting.
   */
  bool pullTransactionsForSlot(uint64_t slot);
  std::chrono::milliseconds txPullWait() const;
  /** Warn (once per epoch) when this miner's account is not a registered miner. */
  void warnIfNotRegisteredMiner();
  /** Re-register with the upstream every REGISTER_RENEW_INTERVAL so the beacon keeps our record. */
  void renewRegistrationPeriodically();
  /** Our registration, signed with all our keys for `networkId` (the beacon verifies it). */
  Roe<Client::MinerInfo> signedRegistration(const std::string &networkId) const;
  Roe<Client::BeaconState> connectToBeacon();
  void initHandlers();
  void handleSlotLeaderRole();
  /** Send a produced (sealed) block to every upstream in parallel. */
  void startBroadcast(const Ledger::ChainNode& block);
  void onBroadcastResult(uint64_t broadcastId, size_t upstream, Client::Roe<Client::TipReply> result);
  /** True when `reply` carries a signed tip, valid here, for exactly `block` (beacon-authority ba4). */
  bool signedTipCovers(const Client::TipReply &reply, const Ledger::ChainNode &block) const;
  /** First upstream accepted it: commit our seal. */
  void commitProducedBlock(const Ledger::ChainNode& block);
  Roe<void> verifyGenesisAnchor();
  /**
   * First block of a fresh miner: the upstream's checkpoint. With an anchored genesis, the
   * checkpoint's authority and admin set must be the ones the upstream's authority proof reaches
   * from it, else the miner starts at genesis, pinned by hash (beacon-authority ba5b).
   */
  Roe<uint64_t> chooseStartingBlock(const Client::BeaconState &state);
  /** Replay the upstream's authority proof from the anchored genesis to `checkpointId` and check the checkpoint agrees. */
  Roe<AuthorityTrail> replayAuthorityTo(uint64_t checkpointId);

  Roe<std::string> hStatus(const Client::Request &request);

  Miner miner_;

  /** A produced block awaiting its broadcast results (at most one). */
  struct PendingBroadcast {
    uint64_t id{0};
    Ledger::ChainNode block;
    BroadcastTally tally{0};
  };
  std::optional<PendingBroadcast> broadcast_;
  uint64_t nextBroadcastId_{1};
  /** Blocks produced and broadcast, committed, abandoned (NodeHealth). */
  uint64_t blocksSealed_{0};
  uint64_t blocksCommitted_{0};
  uint64_t blocksAbandoned_{0};
  /** The last seal, after its slot ended (ms; negative: before). */
  std::optional<int64_t> lastSealDelayMs_;
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
  uint64_t registrationCheckedEpoch_{UINT64_MAX};
  /** Genesis fetched and matched against networkAnchor.genesisHash (none without an anchor). */
  std::optional<Ledger::ChainNode> anchoredGenesis_;
  /** Hash the first block must have (BlockSync::Hooks::firstBlockHash). */
  std::optional<std::string> pinnedFirstBlockHash_;
  /** An anchored start at a checkpoint: the trail the proof reached there. */
  struct AnchoredStart {
    uint64_t blockId{0};
    AuthorityTrail trail;
  };
  std::optional<AnchoredStart> anchoredStart_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_SERVER_H
