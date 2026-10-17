#ifndef PP_LEDGER_FOLLOWER_SERVER_H
#define PP_LEDGER_FOLLOWER_SERVER_H

#include "BlockSync.h"
#include "ClockCalibration.h"
#include "FollowerCore.h"
#include "NodeConfig.h"
#include "Server.h"
#include "UpstreamSet.h"
#include "UpstreamTipWatch.h"
#include "../client/Client.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pp {

/**
 * A server that follows an upstream (RelayServer, MinerServer): it ranks its upstreams and follows
 * the best (UpstreamSet, bp2b), keeps its chain in sync (BlockSync, and the tip watch for each new
 * block), keeps its clock on the beacon's (ClockFollower), passes certificates and equivocation
 * evidence up the tree, and forwards TX_ADD and EQUIVOCATION_ADD to the upstream followed. A block
 * past its tip is answered after the next sync.
 *
 * The role adds what it does with the chain (a relay serves it downstream and forwards writes; a
 * miner produces blocks) through roleDuties() and the hooks below.
 */
class FollowerServer : public Server {
protected:
  /** `name`: this server's logger (its clients log under it). */
  explicit FollowerServer(const std::string &name);

  /**
   * In the role's constructor: the core it follows with (bindCore), and the requests every follower
   * serves (BLOCK_GET past the tip, TX_ADD, EQUIVOCATION_ADD).
   */
  void bindFollower(FollowerCore &core);
  FollowerCore &follower() { return *follower_; }
  const FollowerCore &follower() const { return *follower_; }

  // ----------------- start (onStart) -------------------------------------
  /**
   * The upstreams `config` names (setUpstreams, UpstreamSet), then the AMP server with `identityKey`
   * on its port, serving its allowedPeers (and the upstreams). Keeps `config` (followerConfig).
   */
  Roe<void> startFollowing(const FollowerConfig &config, const std::string &identityKey);
  const FollowerConfig &followerConfig() const { return config_; }
  /** The offset to the beacon's clock (beacon ms = local ms + offset), through the upstream followed, else another. */
  Roe<int64_t> calibrateTimeToBeacon();
  /** Use a new offset to the beacon's clock: the slot clock, and CALIBRATION replies. */
  void applyTimeOffset(int64_t offsetMs);
  /** The certificates' dial hints become upstreams too. True when one was new. */
  bool learnUpstreamHints();
  /**
   * Once the core is open: the clock follower, BlockSync and the tip watch, then the initial
   * catch-up (runStartupSync; `extraDone`: what else to wait for). Empty on success, else why not.
   */
  std::string catchUp(const std::function<bool()> &extraDone = {});

  // ----------------- run -------------------------------------
  /** Follows (rewinds, syncs, tip watch, upstreams, clock), runs roleDuties, serves requests. */
  void runLoop() override;
  /** The role's own work each turn of the run loop, before it serves requests. */
  virtual void roleDuties() {}
  /** Replies to block gets still waiting, then stops (Server::onStop). */
  void onStop() override;
  /** Sync state and readiness: synced, not halted, within kReadyLagBlocks of the upstream's signed tip. */
  void fillHealth(Client::NodeHealth &health) const override;
  int64_t clockOffsetMs() const override { return timeOffsetToBeaconMs_; }

  // ----------------- role hooks -------------------------------------
  /**
   * The hash the first block synced must have (BlockSync::Hooks::firstBlockHash); none: taken on
   * trust. Default: networkAnchor.genesisHash (a node that starts at genesis).
   */
  virtual std::optional<std::string> firstBlockHash() const;
  /** The block a sync starts from carries the authority the node expects (BlockSync::Hooks::checkStartingAuthority). */
  virtual pp::Roe<void> checkStartingAuthority(uint64_t blockId, const ChainAuthority &authority,
                                               const Client::Wallet &admin) const;
  /** A signed tip heard from the upstream (a sync, the tip watch). */
  virtual void onSignedTip(const Client::SignedTip &tip);
  /** A newer certificate or equivocation evidence was kept: what verified before may not now. */
  virtual void onTrustChanged() {}
  /** A sync finished well. */
  virtual void onSynced() {}
  /** A sync now would be refused (a produced block awaits its broadcast): it waits for the next turn. */
  virtual bool syncHeld() const { return false; }
  /** Besides a new epoch, a sync is due now (a miner about to lead a slot). */
  virtual bool syncDue() const { return false; }

  // ----------------- shared -------------------------------------
  /**
   * Whether an upstream serving `state` may be followed: the network anchor (network id, trusted
   * checkpoint) and the network of the chain held.
   */
  Roe<void> admitUpstream(const Client::BeaconState &state) const;
  /** Point `client` at upstream `index` (UpstreamSet::dialKey). */
  Client::Roe<void> dialUpstream(Client &client, size_t index);
  /** Point client_ at the upstream followed now. */
  Client::Roe<void> dialUpstream();
  /** Point client_ at the upstream for a forwarded request; replies with the error if it cannot. */
  bool dialUpstreamFor(const RequestQueue::Reply &reply);
  /**
   * A beacon certificate (STATUS, TX_ADD; ba5c): offer it. A new one goes to every upstream, its
   * dial hints become upstreams and the upstreams are ranked again (bp2b). None when refused.
   */
  std::optional<Chain::CertificateOffer> noteCertificate(const Ledger::Record &record, const std::string &from);
  /** Equivocation evidence (ba6): offer it; a new one goes to every upstream when `pushUp`. None when refused. */
  std::optional<Chain::EquivocationOffer> noteEquivocation(const Client::Equivocation &evidence,
                                                           const std::string &from, bool pushUp);
  /** Start a sync (at most one per slot unless `bypassRateLimit`). */
  void trySyncBlocksFromBeacon(bool bypassRateLimit = false);
  /** A sync is wanted at the next turn of the run loop. */
  void requestSync() { blockSyncRequested_ = true; }
  bool syncInFlight() const { return blockSync_ && blockSync_->inFlight(); }

  /** The upstreams this node may follow, and the one it follows (bp2b). */
  std::unique_ptr<UpstreamSet> upstreams_;
  /** Calls to the upstream followed now (forwarding, STATUS, registration, tip watch). */
  Client client_;
  /** BlockSync's: pointed at an upstream when a sync starts, so a re-rank cannot move a sync midway. */
  Client syncClient_;
  /** Calls to each upstream in turn (STATUS probes, pushes up, a miner's broadcast): each captures the dial key set just before it. */
  Client fanoutClient_;

private:
  /** A relay or miner this many blocks behind its upstream's signed tip is not ready. */
  static constexpr uint64_t kReadyLagBlocks = 3;

  void initUpstreams();
  void initClockFollower();
  void initBlockSync();
  void onBlockSyncFinished(const BlockSync::Result &result);
  /** A sync at each new epoch (the stakeholders), or when the role wants one (syncDue); rate-limited. */
  void syncBlocksPeriodically();
  /** A new certificate, to every upstream (TX_ADD): one behind a stood-down beacon learns it here. */
  void pushCertificateUp(const Ledger::Record &record);
  /** Kept evidence, to every upstream (fire and forget): each beacon host must halt. */
  void pushEquivocationUp(const Client::Equivocation &evidence);

  /** BLOCK_GET: what this node holds now; past its tip, after the next sync. */
  void dBlockGet(const Client::Request &request, const RequestQueue::Reply &reply);
  /** After a sync: answer block gets that were waiting for it. */
  void resolvePendingBlockGets();
  /** TX_ADD: up the tree to the beacon's pool (a beacon certificate is kept here first, ba5c). */
  void dTxAdd(const Client::Request &request, const RequestQueue::Reply &reply);
  /** EQUIVOCATION_ADD (ba6): keep the evidence if it verifies, and pass it up. */
  void dEquivocationAdd(const Client::Request &request, const RequestQueue::Reply &reply);

  FollowerCore *follower_{nullptr};
  FollowerConfig config_;
  /** beacon ms = local ms + offset; 0 until calibrated. */
  int64_t timeOffsetToBeaconMs_{0};
  std::unique_ptr<clock_calibration::ClockFollower> clock_;
  std::unique_ptr<BlockSync> blockSync_;
  /** BLOCK_WAIT on the upstream: requests a sync as soon as it has a new block. */
  std::unique_ptr<UpstreamTipWatch> tipWatch_;
  bool blockSyncRequested_{false};
  std::optional<BlockSync::Result> lastSyncResult_;
  std::chrono::steady_clock::time_point lastBlockSyncTime_{};
  uint64_t lastSyncedEpoch_{0};

  struct PendingBlockGet {
    uint64_t blockId;
    RequestQueue::Reply reply;
  };
  /** Block gets beyond our tip, answered after the next sync. */
  std::vector<PendingBlockGet> pendingBlockGets_;
};

} // namespace pp

#endif // PP_LEDGER_FOLLOWER_SERVER_H
