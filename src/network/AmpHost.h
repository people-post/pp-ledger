#pragma once

#include "InFlightLimit.h"
#include "amp/L1/DatagramIo.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/PeerLinkManager.h"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
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
  /** True on the thread driving the runtime. Blocking on io work from there would deadlock. */
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
   * one in progress. The returned function outlives the host safely.
   */
  template <typename... Args> std::function<void(Args...)> guard(std::function<void(Args...)> fn) const {
    return [gate = gate_, fn = std::move(fn)](Args... args) {
      if (!gate->enter()) {
        return;
      }
      const Gate::Inside inside(*gate);
      fn(std::forward<Args>(args)...);
    };
  }
  IoTask guardTask(IoTask task) const { return guard<>(std::move(task)); }

protected:
  /** Open for pp-ledger: fresh in-flight limits, guarded calls run. */
  void open();
  /** Closed: guarded calls are dropped; waits for one in progress (never call it on the io thread). */
  void close();

private:
  class Gate {
  public:
    /** Leaves on destruction (also when the call throws). */
    struct Inside {
      explicit Inside(Gate& gate) : gate_(gate) {}
      ~Inside() { gate_.leave(); }
      Inside(const Inside&) = delete;
      Inside& operator=(const Inside&) = delete;
      Gate& gate_;
    };
    bool enter();
    void leave();
    void setOpen(bool open);

  private:
    std::mutex mu_;
    std::condition_variable left_;
    bool open_{false};
    size_t inside_{0};
  };

  std::shared_ptr<Gate> gate_;
  std::shared_ptr<InFlightLimit> in_flight_;
  std::shared_ptr<InFlightLimit> bulk_in_flight_;
};

/** In-flight limits to each peer for small and large requests (LedgerRpcProtocol.h, LR007, LR009). */
std::shared_ptr<InFlightLimit> makeInFlightLimit();
std::shared_ptr<InFlightLimit> makeBulkInFlightLimit();

} // namespace network
} // namespace pp
