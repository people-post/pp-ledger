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

/** Optional AMP ingress for pp-ledger Server (when PP_LEDGER_HAS_AMP). */
class ServerAmpSupport {
public:
  using DispatchFn = std::function<std::string(const std::string& requestBody)>;

  ServerAmpSupport() = default;
  ~ServerAmpSupport();

  pp::Service::Roe<void> Start(LedgerAmpConfig config, DispatchFn dispatch, WorkerPool* handler_pool = nullptr);
  void Stop();

  bool isRunning() const { return runtime_.isRunning(); }
  std::string listenMultiaddr() const { return runtime_.listenMultiaddr(); }

private:
  LedgerAmpRuntime runtime_;
  WorkerPool* handler_pool_{nullptr};
};

} // namespace network
} // namespace pp
