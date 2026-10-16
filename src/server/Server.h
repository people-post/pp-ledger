#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "lib/common/Service.h"
#include "../network/ServerAmpSupport.h"
#include "BlockSync.h"
#include "BlockWaitList.h"
#include "RequestQueue.h"
#include "ClockCalibration.h"
#include "UpstreamSet.h"
#include "../network/NetworkTuning.h"
#include "EnvConfig.h"
#include "HealthEndpoint.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <string>

namespace pp {

/**
 * Base for role servers. The role's runLoop thread is the server thread: it
 * owns role state and handles RPC requests one at a time between duties via
 * serveRequestsFor(). See docs/architecture/THREADING.md.
 */
class Server : public Service {
public:
  Server();
  ~Server() override;

  virtual Service::Roe<void> run(const std::string& workDir);

  /**
   * True when run() returned to be run again on a fresh server (beacon-authority bp1, B022): a
   * rewind is applied by a restart. The app builds a new server on the same work directory.
   */
  bool restartRequested() const { return restartRequested_; }

  /**
   * Before run: the operator approves the rewind ordered by beacon generation `generation`, deeper
   * than a node rewinds on its own (`--approve-rewind`; rewind::kMaxAutomaticRewind). 0 = none.
   */
  void setApprovedRewindGeneration(uint64_t generation) { approvedRewindGeneration_ = generation; }

  /**
   * Before run: where the log is also written (`--log-file` / PP_LEDGER_LOG_FILE). Empty: the
   * role's file in the work directory (beacon.log, relay.log, miner.log); "none": stderr only (a
   * container's log driver collects it); else a path (relative: to the work directory).
   */
  void setLogFile(std::string logFile) { logFile_ = std::move(logFile); }

protected:
  virtual bool useSignatureFile() const { return true; }

  static Roe<void> ensureWorkDirectory(const std::string& workDir,
                                       const std::string& signatureFileName,
                                       int32_t errorCode = -1);

  const std::string& getWorkDir() const { return workDir_; }
  virtual std::string getSignatureFileName() const = 0;
  virtual std::string getLogFileName() const = 0;
  virtual std::string getServerName() const = 0;
  virtual int32_t getRunErrorCode() const { return -1; }

  std::string listenMultiaddr() const;
  network::LedgerAmpRuntime* ampRuntime();
  pp::amp::PeerLinkManager* peerLinks();

  static std::string packResponse(const std::string& payload);
  static std::string packResponse(uint16_t errorCode, const std::string& message);

  /** Server thread only. */
  virtual std::string handleParsedRequest(const Client::Request& request) = 0;

  /**
   * Server thread. A role returns true when it takes `request` and will call
   * `reply` later (e.g. after an outbound call completes, via
   * postToServerThread). False: handled synchronously by handleParsedRequest.
   */
  virtual bool handleDeferred(const Client::Request& request, const RequestQueue::Reply& reply);

  /** Any thread: run `task` on the server thread. Dropped once stopped. */
  void postToServerThread(std::function<void()> task);

  /**
   * Wrap a continuation for an async Client call: the result (delivered on the
   * io lane) is handed to `fn` on the server thread.
   */
  template <typename T, typename Fn> std::function<void(Client::Roe<T>)> completeOnServerThread(Fn fn) {
    return [this, fn = std::move(fn)](Client::Roe<T> result) {
      postToServerThread([fn, result = std::move(result)]() mutable { fn(std::move(result)); });
    };
  }

  /** Pack a handler result (Roe<std::string>) into a response and send it. */
  template <typename R> static void replyWith(const RequestQueue::Reply& reply, const R& result) {
    reply(result ? packResponse(result.value()) : packResponse(1, result.error().message));
  }

  /**
   * Server thread: handle queued requests until `budget` elapses (or stop is
   * set). Role runLoops call this in place of sleeping between duties.
   */
  void serveRequestsFor(std::chrono::milliseconds budget);

  /**
   * Server thread, before the run loop serves requests (e.g. onStart): run
   * completion tasks until `done()`, `timeout` or a stop request; requests stay
   * queued. Returns done().
   */
  bool serveTasksUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout);

  /**
   * onStart: run `sync` to completion, retrying failed attempts (BlockSync's
   * backoff) until one succeeds or `timeout` passes — or a restart is requested (a rewind: the
   * node's blocks past a cutover will not sync, and the restart removes them). `lastResult` is set by
   * the role's onFinished hook; `extraDone` lets the role also wait for
   * follow-up work. Returns the last result's error, or "" on success.
   */
  std::string runStartupSync(BlockSync& sync, const std::optional<BlockSync::Result>& lastResult,
                             std::chrono::milliseconds timeout, const std::function<bool()>& extraDone = {});

  /**
   * Io lane: queue for the server thread, or reply busy when full / stopped.
   * `peerId` is the sender's authenticated AMP peer id ("" = unknown).
   */
  void enqueueRequest(std::string body, RequestQueue::Reply reply, std::string peerId = {});

  /** Where a request came from, relative to this node in the ledger tree. */
  enum class Origin { Downstream, Upstream };

  /**
   * Central access rule, checked on the server thread before any handler:
   * may a request of `type` arrive from `origin`? Writes that travel up the
   * tree (BLOCK_ADD, REGISTER, TX_ADD) and requests for what lives upstream
   * (BLOCK_WAIT, MINER_LIST, TX_PULL) are refused from this node's own upstream.
   * See docs/architecture/LEDGER_TOPOLOGY.md (request direction).
   */
  static bool isAllowedFrom(uint32_t type, Origin origin);

  /**
   * Central lane rule: reads (block sync, account and history queries) wait in
   * the Low lane so they cannot crowd out the node's own work. See RequestQueue.
   */
  static RequestQueue::Lane laneFor(uint32_t type);

  /**
   * Before start: this role's upstream endpoints (ADP multiaddrs ending in
   * /p2p/<PeerId>); it follows the first. Requests from the one it follows
   * have Origin::Upstream; all of them pass allowedPeers.
   */
  Service::Roe<void> setUpstreams(const std::vector<std::string>& multiaddrs);
  /** Running: one more upstream (a beacon certificate's dial hint); never this node itself. Any thread. */
  Service::Roe<void> addUpstream(const std::string& multiaddr);
  /**
   * The upstream this node follows now (bp2b, UpstreamSet). Only it is Origin::Upstream: a relay
   * that was below this one may be its candidate upstream and still send it writes. Any thread.
   */
  Service::Roe<void> followUpstream(const std::string& multiaddr);
  /**
   * The UpstreamSet hooks every role wires alike: STATUS through `fanout` (one dial key per
   * upstream), a hint adopted unless it is this node (addUpstream), the one followed tracked
   * (followUpstream). The role adds what it learns and how it judges a tip.
   */
  UpstreamSet::Hooks upstreamHooks(Client& fanout);
  /** `multiaddr` names this node (its peer id; false before the AMP server listens). */
  bool isSelf(const std::string& multiaddr) const;
  /** CALIBRATION from `multiaddr` (dial key `dialKey`) through `client`, dialed before each sample. */
  clock_calibration::Fetch calibrationFetch(Client& client, std::string dialKey, std::string multiaddr);
  /** Before the run loop (start, promotion): estimate through `fetch` and wait for it. */
  Client::Roe<clock_calibration::Offset> calibrateBlocking(const clock_calibration::Fetch& fetch);

  /**
   * Before start: serve only these peers (and this node's upstreams). Unset =
   * anyone. Refused before queueing, so a peer outside the list never takes
   * queue space. Config: `allowedPeers` (parseAllowedPeers).
   */
  void setAllowedPeers(std::optional<std::set<std::string>> peerIds) { allowedPeers_ = std::move(peerIds); }
  bool isAllowedPeer(const std::string& peerId) const;
  /** Optional `allowedPeers` array of peer ids in a role's config.json; absent = nullopt. */
  static Service::Roe<std::optional<std::set<std::string>>> parseAllowedPeers(const pp::common::Object& config);

  /** The probe / metrics endpoint (`healthPort`, `healthBind`; PP_LEDGER_HEALTH_PORT / _BIND). */
  struct HealthConfig {
    std::optional<uint16_t> port; ///< none: no endpoint
    std::string bind{"127.0.0.1"};
  };
  static Service::Roe<HealthConfig> parseHealthConfig(const pp::common::Object& config);
  /** In onStart, right after the config: probes answer during the startup sync (not ready yet). */
  Service::Roe<void> startHealthEndpoint(const HealthConfig& config);
  /**
   * Server thread: what probes and /metrics read. The base fills identity and the queue; roles add
   * their chain state and readiness (call the base first).
   */
  virtual void fillHealth(HealthSnapshot& snapshot) const;
  /** True once the run loop serves requests (onStart is over). */
  bool isServing() const { return serving_; }
  Origin originOf(const std::string& peerId) const;

  /**
   * Network policy: fixed defaults (NetworkTuning), not operator config; tests
   * override it before start. Sizes the request queue and its expiry and is
   * applied to AMP. Roles read it back via networkTuning().
   */
  void setNetworkTuning(const network::NetworkTuning& tuning);
  const network::NetworkTuning& networkTuning() const { return tuning_; }

  /** Before start only (tests): queue capacity and max time a request may wait. */
  void setRequestLimits(size_t capacity, std::chrono::milliseconds maxWait);

  /**
   * This node's network identity key (`keys/amp-identity.txt` in the work
   * dir), created on first start. Beacon and relay; a miner's identity is its
   * account key.
   */
  Service::Roe<std::string> loadOrCreateIdentityKey();

  Service::Roe<void> startAmpServer(const network::LedgerAmpConfig& config);
  void stopAmpServer();
  bool isAmpServerRunning() const { return ampSupport_ && ampSupport_->isRunning(); }

  void onStop() override;

  /**
   * Nodes that serve downstream sync (beacon, relay) return their next block
   * id so the base can answer BLOCK_WAIT; nullopt (default) = not served.
   */
  virtual std::optional<uint64_t> blockWaitTip() const { return std::nullopt; }
  /** The beacon's latest signed tip this node holds, sent with each BLOCK_WAIT answer. */
  virtual std::optional<Client::SignedTip> latestSignedTip() const { return std::nullopt; }

  uint64_t approvedRewindGeneration() const { return approvedRewindGeneration_; }

  /** How long a node keeps serving after it ordered its own rewind, before it restarts. */
  static constexpr std::chrono::milliseconds kRewindRestartGrace{2000};

  /**
   * Stop the run loop after `grace` — time for requests in flight (a forward upstream) to finish —
   * so run() returns with restartRequested() set. Safe during onStart: serveRequestsFor stops the
   * loop once the grace has passed.
   */
  void requestRestart(std::chrono::milliseconds grace = std::chrono::milliseconds(0)) {
    restartAt_ = RequestQueue::Clock::now() + grace;
    restartRequested_ = true;
  }

private:
  /** Server thread: one queued request — deferred, or handled now and replied. */
  void serveRequest(const RequestQueue::Item& item);
  /** BLOCK_WAIT: answer now if the tip is past the caller's, else park it. */
  void serveBlockWait(const Client::Request& request, const RequestQueue::Item& item);
  /** Answer parked waits the tip has passed or whose hold ended (cheap when nothing changed). */
  void pollBlockWaits();
  void answerBlockWaits(std::vector<RequestQueue::Reply> replies, uint64_t tip);
  /** Refuse new requests and reply to pending ones; runs before AMP stops. */
  void closeRequestQueue();

  std::string workDir_;
  std::unique_ptr<network::ServerAmpSupport> ampSupport_;
  network::NetworkTuning tuning_;
  /** Change while running (addUpstream, followUpstream); read on the io lane and the server thread. */
  mutable std::mutex upstreamMutex_;
  std::set<std::string> upstreamPeerIds_;
  std::string followedPeerId_;
  /** Fixed before start; read on the io lane without locking. */
  std::optional<std::set<std::string>> allowedPeers_;
  size_t requestCapacity_{tuning_.requestQueueCapacity};
  /** A request older than this was given up on by its client: reply without doing the work. */
  std::chrono::milliseconds maxRequestWait_{tuning_.serverQueueExpiry()};
  std::string logFile_;
  std::unique_ptr<HealthEndpoint> health_;
  std::chrono::steady_clock::time_point healthPublishedAt_{};
  bool serving_{false};
  /** Server thread: publish the health snapshot, at most once a second. */
  void publishHealth();
  std::unique_ptr<RequestQueue> requests_;
  BlockWaitList blockWaits_{requestCapacity_};
  uint64_t lastPolledTip_{0};
  RequestQueue::Clock::time_point nextBlockWaitSweep_{};
  std::atomic<bool> restartRequested_{false};
  uint64_t approvedRewindGeneration_{0};
  RequestQueue::Clock::time_point restartAt_{};
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
