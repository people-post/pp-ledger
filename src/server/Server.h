#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "ledger/client/Client.h"
#include "ledger/lib/common/Service.h"
#include "network/ServerAmpSupport.h"
#include "server/BlockSync.h"
#include "server/BlockWaitList.h"
#include "server/NodeCore.h"
#include "server/RequestQueue.h"
#include "server/ClockCalibration.h"
#include "server/UpstreamSet.h"
#include "ledger/network/NetworkTuning.h"
#include "server/EnvConfig.h"
#include "server/NodeConfig.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
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
  /** Error codes of a role server (Service::Error): config, network, the role's core, a request. */
  static constexpr int32_t E_CONFIG = -1;
  static constexpr int32_t E_NETWORK = -2;
  static constexpr int32_t E_ROLE = -3;
  static constexpr int32_t E_REQUEST = -4;
  /** When a busy node asks a client to try again (Client::E_BUSY). */
  static constexpr uint32_t kBusyRetryAfterMs = 1000;

  Server();
  ~Server() override;

  virtual Service::Roe<void> run(const std::string& workDir);
  /**
   * Before run: serve on a host's Amp runtime — pp-node's mesh — instead of a socket of its own
   * (platform-integration pi1). The node then has the host's identity and listen address; its
   * configured port and identity key are not used, and the host keeps the runtime-wide settings.
   */
  void runOnHost(network::HostedAmpRuntime::Host host) { hostAmp_ = std::move(host); }
  bool onHost() const { return hostAmp_.runtime != nullptr; }
  /**
   * Before run: told on the node's thread when its ledger protocols are bound (true — opens are
   * served from now on: when a host advertises AmpLedgerServer::protocolIds()) and when they are
   * about to be unbound (false, at stop — a host withdraws them first). A host's mesh refuses opens
   * for protocols nothing serves, so advertising before `true` only earns peers refusals.
   */
  void setOnListening(std::function<void(bool listening)> onListening) { onListening_ = std::move(onListening); }

  /**
   * True when run() returned to be run again on a fresh server (beacon-authority bp1, B022): a
   * rewind is applied by a restart. The app builds a new server on the same work directory.
   */
  bool restartRequested() const { return restartRequested_; }

  /**
   * Before run: the operator approves the rewind ordered by beacon generation `generation` when
   * it drops final blocks (`--approve-rewind`; projects/finality FN003). 0 = none.
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
  virtual int32_t getRunErrorCode() const { return E_ROLE; }

  std::string listenMultiaddr() const;
  /** The Amp runtime the node serves and its clients call on (its own or a host's); none before start. */
  network::AmpHost* ampRuntime();
  pp::amp::PeerLinkManager* peerLinks();


  static std::string packResponse(const std::string& payload);
  static std::string packResponse(uint16_t errorCode, const std::string& message);
  /** An overload or deadline answer (Client::E_BUSY …, load-resilience LR007), with when to try again. */
  static std::string packError(uint16_t code, const std::string& message, uint32_t retryAfterMs = 0);


  /**
   * In the role's constructor: the core this server serves. The base then answers the chain reads
   * (BLOCK_GET, ACCOUNT_GET, TX_GET_BY_WALLET, TX_GET_BY_INDEX, AUTHORITY_PROOF, CALIBRATION), fills
   * the chain's part of HEALTH, and orders the rewinds the core schedules.
   */
  void bindCore(NodeCore& core);
  NodeCore& core() { return *core_; }
  const NodeCore& core() const { return *core_; }

  /** Server thread: a handler's result, packed by the base (an error is code 1 with its message). */
  using Handler = std::function<Roe<std::string>(const Client::Request& request)>;
  /** Server thread: a handler that replies itself, now or later (after an outbound call, via postToServerThread). */
  using LaterHandler = std::function<void(const Client::Request& request, const RequestQueue::Reply& reply)>;
  /** Serve request `type` with `handler` (replacing any handler of `type`). */
  void handle(uint32_t type, Handler handler);
  void handleLater(uint32_t type, LaterHandler handler);
  /**
   * Server thread, before the handler of an allowed request: refuse it with an error (for example,
   * writes to a beacon that stood down). Default: admit.
   */
  virtual Roe<void> admit(const Client::Request& request) const;
  /** CALIBRATION: this node's clock against the beacon's, in ms (a follower's calibration). */
  virtual int64_t clockOffsetMs() const { return 0; }
  /** CALIBRATION's reply (the default handler; a role may refuse first). */
  Roe<std::string> calibrationReply() const;

  /**
   * Server thread: when the held certificate orders a rewind (beacon-authority bp1;
   * NodeCore::scheduleRewind), write the order and restart after kRewindRestartGrace. Called once
   * the role has loaded, when a sync finishes (startup syncs too: a node back online with blocks
   * past the cutover cannot sync), and from the run loop.
   */
  void orderRewindIfDue();

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
   * backoff) until one succeeds or `timeout` passes with no block authenticated or applied
   * (BlockSync::blocksProcessed): a long chain synced from genesis takes as long as it takes, a
   * stuck one fails (load-resilience lr9) — or a restart is requested (a rewind: the
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
   * queue space; a large request when its channel opens, before its body
   * arrives. Config: `allowedPeers` (NodeConfig).
   */
  void setAllowedPeers(std::optional<std::set<std::string>> peerIds) { allowedPeers_ = std::move(peerIds); }
  bool isAllowedPeer(const std::string& peerId) const;
  /** io lane: the reply to a request refused when its channel opened (counted busy or refused). */
  std::string refusalResponse(network::AmpLedgerServer::Refusal why);
  /**
   * A peer this node's chain work depends on (load-resilience LR009): a miner registered through it
   * (for a while after), an upstream, a peer listed in `allowedPeers`. Its requests have their own
   * queue lane, served first, and large ones their own places: a flood from strangers cannot hold up
   * a miner's sync or block. Any thread.
   */
  bool isTrustedPeer(const std::string& peerId) const;
  /**
   * Server thread: miner `minerId`'s registration, accepted by the beacon, names `peerId` (signed)
   * and came from it; that peer is trusted until it lapses. One peer per miner: a newer
   * registration's peer replaces the one before.
   */
  void noteMinerPeer(uint64_t minerId, const std::string& peerId);
  /** This node's AMP peer id (from its listen address; "" before it listens). */
  std::string localPeerId() const;
  /** A registered miner's peer stays trusted this long without registering again (miners renew sooner). */
  static constexpr std::chrono::minutes kMinerPeerTtl{10};

  /**
   * onStart: `config.json` in the work directory merged with the PP_* variables `fields`
   * (env_config::loadConfig). With neither, `templateJson` is written there for the operator to fill
   * in and the start stops, telling them `whatToSet`.
   */
  Roe<pp::common::Object> loadRunConfig(const std::vector<env_config::Field>& fields,
                                        const pp::common::Object& templateJson, const std::string& whatToSet);

  /**
   * Server thread: what HEALTH answers (Client::NodeHealth). The base fills identity, the queue and
   * the core's chain state; roles add their readiness (call the base first).
   */
  virtual void fillHealth(Client::NodeHealth& health) const;
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

  /** Before startAmpServer: the link table, handshake backlog (LR005) and socket buffers from the role's config. */
  void setTransportLimits(const NodeConfig& config) {
    maxLinks_ = config.maxLinks;
    handshakeBacklog_ = config.handshakeBacklog;
    udpBufferBytes_ = config.udpBufferBytes;
  }
  /** This node's network identity key (a role's own choice: loadOrCreateIdentityKey, a miner's account key). */
  using IdentityKeyFn = std::function<Service::Roe<std::string>()>;
  /**
   * Serve ledger RPC: on the host's runtime (runOnHost), else on a runtime of its own on UDP `port`
   * with `identityKey` (asked only then).
   */
  Service::Roe<void> startAmpServer(uint16_t port, const IdentityKeyFn& identityKey);
  /** startAmpServer on a runtime of its own: socket, identity, link limits and buffers from the config. */
  Service::Roe<void> startOwnAmp(uint16_t port, const IdentityKeyFn& identityKey,
                                 network::ServerAmpSupport::DispatchFn dispatch,
                                 network::AmpLedgerServer::Options ingress);
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
  /** Server thread: one queued request, through its handler. */
  void serveRequest(const RequestQueue::Item& item);
  /** The chain reads every core answers (bindCore). */
  void handleChainReads();
  /** BLOCK_WAIT: answer now if the tip is past the caller's, else park it. */
  void serveBlockWait(const Client::Request& request, const RequestQueue::Item& item);
  /** Answer parked waits the tip has passed or whose hold ended (cheap when nothing changed). */
  void pollBlockWaits();
  void answerBlockWaits(std::vector<RequestQueue::Reply> replies, uint64_t tip);
  /** Refuse new requests and reply to pending ones; runs before AMP stops. */
  void closeRequestQueue();

  std::string workDir_;
  NodeCore* core_{nullptr};
  struct Route {
    Handler now;
    LaterHandler later;
  };
  std::map<uint32_t, Route> routes_;
  std::unique_ptr<network::ServerAmpSupport> ampSupport_;
  network::HostedAmpRuntime::Host hostAmp_;
  std::function<void(bool listening)> onListening_;
  uint32_t maxLinks_{NodeConfig::kDefaultMaxLinks};
  uint32_t handshakeBacklog_{NodeConfig::kDefaultHandshakeBacklog};
  uint32_t udpBufferBytes_{NodeConfig::kDefaultUdpBufferBytes};
  network::NetworkTuning tuning_;
  /** Change while running (addUpstream, followUpstream); read on the io lane and the server thread. */
  mutable std::mutex upstreamMutex_;
  std::set<std::string> upstreamPeerIds_;
  std::string followedPeerId_;
  /** Fixed before start; read on the io lane without locking. */
  std::optional<std::set<std::string>> allowedPeers_;
  /** Peers that registered a miner through this node, and until when they are trusted. */
  mutable std::mutex minerPeersMutex_;
  std::map<std::string, std::chrono::steady_clock::time_point> minerPeers_;
  /** The peer each miner's latest registration named (one trusted peer per miner). */
  std::map<uint64_t, std::string> minerPeerOf_;
  size_t requestCapacity_{tuning_.requestQueueCapacity};
  /** A request older than this was given up on by its client: reply without doing the work. */
  std::chrono::milliseconds maxRequestWait_{tuning_.serverQueueExpiry()};
  std::string logFile_;
  /** The last published HEALTH, answered on the io lane (enqueueRequest) without queueing. */
  mutable std::mutex healthMutex_;
  std::optional<Client::NodeHealth> health_;
  std::chrono::steady_clock::time_point healthPublishedAt_{};
  bool serving_{false};
  /** Server thread: publish the health snapshot, at most once a second. */
  void publishHealth();
  /** io lane: the HEALTH reply, from the last snapshot. */
  std::string healthResponse() const;
  /** `<work dir>/listen.multiaddr`: where a local `pp-client health` reaches this node. */
  void writeListenMultiaddrFile() const;
  std::unique_ptr<RequestQueue> requests_;
  /** Load counters (NodeHealth, load-resilience lr1). Busy and refused are counted on the io lane too. */
  std::atomic<uint64_t> requestsBusy_{0};
  std::atomic<uint64_t> requestsRefused_{0};
  uint64_t requestsExpired_{0};
  std::map<uint32_t, Client::NodeHealth::RequestCount> requestCounts_;
  /** Run-loop turns: one per serveRequestsFor; the longest kept per 10 s window (this one and the last). */
  std::optional<RequestQueue::Clock::time_point> lastTurnAt_;
  uint64_t loopTurnsSlow_{0};
  RequestQueue::Clock::time_point turnWindowStart_{};
  uint64_t turnMaxMs_{0};
  uint64_t lastWindowTurnMaxMs_{0};
  /** Server thread, at each run-loop turn. */
  void noteLoopTurn(RequestQueue::Clock::time_point now);
  BlockWaitList blockWaits_{requestCapacity_};
  uint64_t lastPolledTip_{0};
  RequestQueue::Clock::time_point nextBlockWaitSweep_{};
  std::atomic<bool> restartRequested_{false};
  uint64_t approvedRewindGeneration_{0};
  RequestQueue::Clock::time_point restartAt_{};
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
