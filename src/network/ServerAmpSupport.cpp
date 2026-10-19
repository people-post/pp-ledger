#include "ledger/network/ServerAmpSupport.h"

#include "ledger/lib/common/Service.h"

namespace pp {
namespace network {

ServerAmpSupport::~ServerAmpSupport() { Stop(); }

pp::Service::Roe<void> ServerAmpSupport::Start(LedgerAmpConfig config, DispatchFn dispatch,
                                               AmpLedgerServer::Options ingress) {
  const auto read_timeout = config.rpc_read_timeout;
  auto runtime = std::make_unique<LedgerAmpRuntime>();
  if (auto started = runtime->Start(std::move(config)); !started) {
    return pp::Service::Error(-1, started.error().message);
  }
  own_ = runtime.get();
  host_ = std::move(runtime);
  ingress.read_timeout = read_timeout;
  bind(std::move(dispatch), std::move(ingress));
  return {};
}

pp::Service::Roe<void> ServerAmpSupport::StartHosted(HostedAmpRuntime::Host host,
                                                     const std::chrono::milliseconds readTimeout, DispatchFn dispatch,
                                                     AmpLedgerServer::Options ingress) {
  if (!host.runtime) {
    return pp::Service::Error(-1, "No host Amp runtime");
  }
  auto hosted = std::make_unique<HostedAmpRuntime>(std::move(host));
  hosted_ = hosted.get();
  host_ = std::move(hosted);
  ingress.read_timeout = readTimeout;
  ingress.advertise = false; // the host advertises AmpLedgerServer::protocolIds() with its own
  bind(std::move(dispatch), std::move(ingress));
  return {};
}

void ServerAmpSupport::bind(DispatchFn dispatch, AmpLedgerServer::Options ingress) {
  // ChannelSession / Mux are io-thread affine — send replies on the io lane (post() wakes a pump of
  // its own). Requests reach the server only while the host is open.
  ingress.post_io = [host = host_.get()](std::function<void()> task) { host->post(std::move(task)); };
  AmpLedgerServer::BindAsync(host_->links(), host_->guard(std::move(dispatch)), std::move(ingress));
}

void ServerAmpSupport::Stop() {
  if (!host_) {
    return;
  }
  // Handlers go first (an open in progress finishes under the io lock), then the gate: nothing from
  // the io lane reaches the server afterwards.
  AmpLedgerServer::Unbind(host_->links());
  if (hosted_) {
    hosted_->closeHosted();
  }
  if (own_) {
    own_->Stop();
  }
  host_.reset();
  own_ = nullptr;
  hosted_ = nullptr;
}

} // namespace network
} // namespace pp
