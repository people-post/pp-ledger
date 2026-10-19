#pragma once

#include "ledger/network/AmpLedgerServer.h"
#include "ledger/network/HostedAmpRuntime.h"
#include "ledger/network/LedgerAmpRuntime.h"
#include "ledger/lib/common/Service.h"

#include <functional>
#include <memory>
#include <string>

namespace pp {

class Server;

namespace network {

/**
 * AMP ingress for pp-ledger Server: /pp-ledger/rpc on its own runtime (a standalone node) or on a
 * host's (platform-integration pi1). Either way the server's clients run on host().
 */
class ServerAmpSupport {
public:
  /** Runs on the io lane; must not block. Reply now or hand the request off. */
  using DispatchFn = AmpLedgerServer::AsyncHandler;

  ServerAmpSupport() = default;
  ~ServerAmpSupport();

  /** Standalone: a runtime of its own on `config`'s socket and identity. `ingress`: who may send large
   *  requests, and the replies to refused ones (its io lane and read timeout are this runtime's). */
  pp::Service::Roe<void> Start(LedgerAmpConfig config, DispatchFn dispatch, AmpLedgerServer::Options ingress = {});
  /** Hosted: the protocol handlers on the host's runtime; the host keeps its runtime-wide settings. */
  pp::Service::Roe<void> StartHosted(HostedAmpRuntime::Host host, std::chrono::milliseconds readTimeout,
                                     DispatchFn dispatch, AmpLedgerServer::Options ingress = {});
  /** Unbind, then no call into the server from the io lane runs any more (and a standalone runtime stops). */
  void Stop();

  bool isRunning() const { return host_ && host_->isRunning(); }
  std::string listenMultiaddr() const { return host_ ? host_->listenMultiaddr() : std::string{}; }
  /** While started. */
  AmpHost& host() { return *host_; }
  pp::amp::PeerLinkManager& links() { return host_->links(); }
  /** The runtime this ingress owns; none when hosted. */
  LedgerAmpRuntime* ownRuntime() { return own_; }

private:
  void bind(DispatchFn dispatch, AmpLedgerServer::Options ingress);

  std::unique_ptr<AmpHost> host_;
  LedgerAmpRuntime* own_{nullptr};
  HostedAmpRuntime* hosted_{nullptr};
};

} // namespace network
} // namespace pp
