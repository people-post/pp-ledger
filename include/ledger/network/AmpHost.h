#pragma once

#include "ledger/network/InFlightLimit.h"
#include "amp/L1/DatagramIo.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/PeerLinkManager.h"
#include "common/Lifetime.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace pp {
namespace network {

/**
 * The Amp runtime pp-ledger's servers and clients run on (platform-integration pi1): their own
 * (LedgerAmpRuntime: a standalone node's socket, identity and pump thread) or a host's
 * (HostedAmpRuntime: pp-node's mesh, one socket and identity for everything the node runs).
 *
 * pp-ledger never drives the runtime: it posts work onto its io lane (post) and registers its
 * protocol handlers on its links. Calls from the io lane into pp-ledger code go through guard(), so
 * once close() returns none is running or will run — the host's runtime may outlive pp-ledger's
 * servers.
 */
class AmpHost {
public:
  using IoTask = std::function<void()>;

  AmpHost();
  virtual ~AmpHost();

  AmpHost(const AmpHost&) = delete;
  AmpHost& operator=(const AmpHost&) = delete;

  virtual bool isRunning() const = 0;
  /**
   * Setup-time access (bind handlers, register endpoints) and calls the runtime makes thread-safe.
   * Per-request link or channel work is post()ed.
   */
  virtual pp::amp::MeshRuntime& runtime() = 0;
  pp::amp::PeerLinkManager& links() { return runtime().Links(); }
  /** Run `task` on the io lane, guarded (see guard). Any thread; dropped once closed or stopped. */
  virtual void post(IoTask task) = 0;
  /**
   * True where io work runs: the thread driving the runtime, or one holding its io lock through its
   * io executor (MeshRuntime::IoExecutor). Blocking on io work from there would deadlock.
   */
  virtual bool onIoThread() const = 0;
  /** This node's listen multiaddr, ending in /p2p/<PeerId>; empty when not running. */
  virtual std::string listenMultiaddr() const = 0;
  /** The socket's buffers and drops: only a runtime that owns its socket knows them. */
  virtual std::optional<pp::adp::DatagramIoStats> socketStats() const { return std::nullopt; }
  /**
   * Runtime-wide settings — advertised protocols, listen addresses, refusing unhandled opens, link
   * limits — are pp-ledger's to set only on its own runtime; a host keeps them (it advertises
   * pp-ledger's protocols itself: AmpLedgerServer::protocolIds).
   */
  virtual bool ownsRuntime() const = 0;

  /**
   * io lane: requests in flight to each peer from every client on this host (InFlightLimit),
   * small and large apart; fresh at each open.
   */
  const std::shared_ptr<InFlightLimit>& inFlightLimit() const { return in_flight_; }
  const std::shared_ptr<InFlightLimit>& bulkInFlightLimit() const { return bulk_in_flight_; }

  /**
   * `fn`, run only while this host is open: a call after close() is dropped, and close() waits for
   * one in progress on another thread. The returned function outlives the host safely.
   */
  template <typename... Args> std::function<void(Args...)> guard(std::function<void(Args...)> fn) const {
    return lifetime_.Bind([open = open_, fn = std::move(fn)](Args... args) {
      if (open->load(std::memory_order_acquire)) {
        fn(std::forward<Args>(args)...);
      }
    });
  }
  IoTask guardTask(IoTask task) const { return guard<>(std::move(task)); }

protected:
  /** Open for pp-ledger: fresh in-flight limits, guarded calls run. */
  void open();
  /** Closed: guarded calls are dropped; waits for one in progress on another thread. */
  void close();

private:
  /** Read inside each guarded call: one that saw it open is still running when close() waits. */
  std::shared_ptr<std::atomic<bool>> open_ = std::make_shared<std::atomic<bool>>(false);
  pp::Lifetime lifetime_;
  std::shared_ptr<InFlightLimit> in_flight_;
  std::shared_ptr<InFlightLimit> bulk_in_flight_;
};

/** In-flight limits to each peer for small and large requests (LedgerRpcProtocol.h, LR007, LR009). */
std::shared_ptr<InFlightLimit> makeInFlightLimit();
std::shared_ptr<InFlightLimit> makeBulkInFlightLimit();

} // namespace network
} // namespace pp
