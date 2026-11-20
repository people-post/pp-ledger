#pragma once

#include "ledger/network/AmpHost.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace pp {
namespace network {

/**
 * An AmpHost over a runtime someone else owns and drives — pp-node's mesh (platform-integration
 * pi1): one socket and one identity for the node and its ledger role. The host keeps the
 * runtime-wide settings (advertisement, listen addresses, link limits) and its driver thread;
 * pp-ledger posts onto its io lane and binds its own protocol handlers.
 *
 * Open from construction to close(): close it (from a thread other than the host's driver) before
 * the pp-ledger objects using it go away, while the host's runtime still runs or after it stopped.
 */
class HostedAmpRuntime : public AmpHost {
public:
  struct Host {
    /** The host's runtime; it must outlive this object. */
    pp::amp::MeshRuntime* runtime{nullptr};
    /** This node's listen multiaddr, ending in /p2p/<PeerId> (the host's may change). */
    std::function<std::string()> listenMultiaddr;
  };

  explicit HostedAmpRuntime(Host host);
  ~HostedAmpRuntime() override;

  /** Stop using the host's runtime: guarded calls are dropped from here on (AmpHost::close). */
  void closeHosted();

  /** Open (the host's own started state is its driver's to read). */
  bool isRunning() const override { return open_; }
  pp::amp::MeshRuntime& runtime() override { return *host_.runtime; }
  void post(IoTask task) override;
  /** The host's io lane (MeshRuntime::IoExecutor): its driver thread, or inside its RunAndWait. */
  bool onIoThread() const override;
  std::string listenMultiaddr() const override;
  bool ownsRuntime() const override { return false; }

private:
  Host host_;
  /** The host's io lane; outlives its runtime safely (posts are then dropped). */
  std::shared_ptr<pp::SerialExecutor> io_;
  std::atomic<bool> open_{false};
};

} // namespace network
} // namespace pp
