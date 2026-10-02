#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "lib/common/Service.h"
#include "../network/ServerAmpSupport.h"
#include "BlockSync.h"
#include "BlockWaitList.h"
#include "RequestQueue.h"
#include "../network/NetworkTuning.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
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
   * completion tasks until `done()` or `timeout`; requests stay queued.
   * Ignores the stop flag (still set during onStart). Returns done().
   */
  bool serveTasksUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout);

  /**
   * onStart: run `sync` to completion, retrying failed attempts (BlockSync's
   * backoff) until one succeeds or `timeout` passes. `lastResult` is set by
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
   * tree (BLOCK_ADD, REGISTER) are refused from this node's own upstream.
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
   * /p2p/<PeerId>). Requests from those peers have Origin::Upstream.
   */
  Service::Roe<void> setUpstreams(const std::vector<std::string>& multiaddrs);
  Origin originOf(const std::string& peerId) const;

  /**
   * Before start: operator network policy (config.json `network`). Sizes the
   * request queue, sets its expiry, and is applied to AMP and the role's
   * clients. Roles read it back via networkTuning().
   */
  void setNetworkTuning(const network::NetworkTuning& tuning);
  const network::NetworkTuning& networkTuning() const { return tuning_; }

  /** Before start only (tests): queue capacity and max time a request may wait. */
  void setRequestLimits(size_t capacity, std::chrono::milliseconds maxWait);

  Service::Roe<void> startAmpServer(const network::LedgerAmpConfig& config);
  void stopAmpServer();
  bool isAmpServerRunning() const { return ampSupport_ && ampSupport_->isRunning(); }

  void onStop() override;

  /**
   * Nodes that serve downstream sync (beacon, relay) return their next block
   * id so the base can answer BLOCK_WAIT; nullopt (default) = not served.
   */
  virtual std::optional<uint64_t> blockWaitTip() const { return std::nullopt; }

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
  std::set<std::string> upstreamPeerIds_;
  size_t requestCapacity_{tuning_.requestQueueCapacity};
  /** A request older than this was given up on by its client: reply without doing the work. */
  std::chrono::milliseconds maxRequestWait_{tuning_.serverQueueExpiry()};
  std::unique_ptr<RequestQueue> requests_;
  BlockWaitList blockWaits_{requestCapacity_};
  uint64_t lastPolledTip_{0};
  RequestQueue::Clock::time_point nextBlockWaitSweep_{};
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
