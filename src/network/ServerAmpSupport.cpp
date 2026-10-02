#include "ServerAmpSupport.h"

#include "lib/common/Service.h"

namespace pp {
namespace network {

ServerAmpSupport::~ServerAmpSupport() { Stop(); }

pp::Service::Roe<void> ServerAmpSupport::Start(LedgerAmpConfig config, DispatchFn dispatch) {
  const auto read_timeout = config.rpc_read_timeout;
  auto started = runtime_.Start(std::move(config));
  if (!started) {
    return pp::Service::Error(-1, started.error().message);
  }
  // ChannelSession / Mux are io-thread affine — send replies on the Amp pump (post() wakes it).
  AmpLedgerServer::IoPost post_io = [this](std::function<void()> task) { runtime_.post(std::move(task)); };
  AmpLedgerServer::BindAsync(runtime_.links(), std::move(dispatch), std::move(post_io), read_timeout);
  return {};
}

void ServerAmpSupport::Stop() {
  if (runtime_.isRunning()) {
    AmpLedgerServer::Unbind(runtime_.links());
  }
  runtime_.Stop();
}

} // namespace network
} // namespace pp
