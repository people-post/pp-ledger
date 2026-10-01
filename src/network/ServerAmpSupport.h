#pragma once

#include "AmpLedgerServer.h"
#include "LedgerAmpRuntime.h"
#include "lib/common/Service.h"

#include <functional>
#include <memory>
#include <string>

namespace pp {

class Server;

namespace network {

/** AMP ingress for pp-ledger Server. */
class ServerAmpSupport {
public:
  /** Runs on the io lane; must not block. Reply now or hand the request off. */
  using DispatchFn = AmpLedgerServer::AsyncHandler;

  ServerAmpSupport() = default;
  ~ServerAmpSupport();

  pp::Service::Roe<void> Start(LedgerAmpConfig config, DispatchFn dispatch);
  void Stop();

  bool isRunning() const { return runtime_.isRunning(); }
  std::string listenMultiaddr() const { return runtime_.listenMultiaddr(); }
  LedgerAmpRuntime& runtime() { return runtime_; }
  pp::amp::PeerLinkManager& links() { return runtime_.links(); }

private:
  LedgerAmpRuntime runtime_;
};

} // namespace network
} // namespace pp
