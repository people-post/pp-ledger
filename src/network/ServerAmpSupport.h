#pragma once

#include "AmpLedgerServer.h"
#include "LedgerAmpRuntime.h"
#include "lib/common/Service.h"
#include "common/WorkerPool.h"

#include <functional>
#include <memory>
#include <string>

namespace pp {

class Server;

namespace network {

/** AMP ingress for pp-ledger Server. */
class ServerAmpSupport {
public:
  using DispatchFn = std::function<std::string(const std::string& requestBody)>;

  ServerAmpSupport() = default;
  ~ServerAmpSupport();

  pp::Service::Roe<void> Start(LedgerAmpConfig config, DispatchFn dispatch, WorkerPool* handler_pool = nullptr);
  void Stop();

  bool isRunning() const { return runtime_.isRunning(); }
  std::string listenMultiaddr() const { return runtime_.listenMultiaddr(); }
  LedgerAmpRuntime& runtime() { return runtime_; }
  pp::amp::PeerLinkManager& links() { return runtime_.links(); }
  LedgerAmpRuntime::IoPump ioPump() const { return runtime_.ioPump(); }

private:
  LedgerAmpRuntime runtime_;
  WorkerPool* handler_pool_{nullptr};
};

} // namespace network
} // namespace pp
