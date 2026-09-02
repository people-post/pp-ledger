#include "ServerAmpSupport.h"

#include "lib/common/Service.h"

namespace pp {
namespace network {

ServerAmpSupport::~ServerAmpSupport() { Stop(); }

pp::Service::Roe<void> ServerAmpSupport::Start(LedgerAmpConfig config, DispatchFn dispatch, WorkerPool* handler_pool) {
  handler_pool_ = handler_pool;
  auto started = runtime_.Start(std::move(config));
  if (!started) {
    return pp::Service::Error(-1, started.error().message);
  }

  AmpLedgerServer::WorkerPost post_worker;
  if (handler_pool_) {
    post_worker = [this](std::function<void()> task) {
      handler_pool_->Post(WorkerLane::Normal, std::move(task));
    };
  }

  AmpLedgerServer::Bind(runtime_.links(), std::move(dispatch), std::move(post_worker));
  return {};
}

void ServerAmpSupport::Stop() {
  if (runtime_.isRunning()) {
    AmpLedgerServer::Unbind(runtime_.links());
  }
  runtime_.Stop();
  handler_pool_ = nullptr;
}

} // namespace network
} // namespace pp
