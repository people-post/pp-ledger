#include "LedgerAmpRuntime.h"

#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"

namespace pp {
namespace network {

LedgerAmpRuntime::~LedgerAmpRuntime() { Stop(); }

pp::Roe<void> LedgerAmpRuntime::Start(LedgerAmpConfig config) {
  auto clock = std::make_shared<pp::adp::WallClock>();
  const auto local = pp::adp::IpEndpoint::V4(0, 0, 0, 0, config.udp_port);
  auto bound = pp::adp::OsUdpDatagramIo::Bind(local);
  if (!bound) {
    return bound.error();
  }
  std::shared_ptr<pp::adp::DatagramIo> io(std::move(*bound));
  return StartForTest(std::move(io), std::move(clock), std::move(config));
}

pp::Roe<void> LedgerAmpRuntime::StartForTest(std::shared_ptr<pp::adp::DatagramIo> io,
                                             std::shared_ptr<pp::adp::Clock> clock, LedgerAmpConfig config) {
  if (running_) {
    return pp::Error("LedgerAmpRuntime: already running");
  }
  io_ = std::move(io);
  clock_ = std::move(clock);

  pp::amp::AmpStack::Config stack_config;
  stack_config.identity = std::move(config.identity);
  stack_config.local_peer_id = std::move(config.local_peer_id);
  stack_config.link_config = std::move(config.link_config);

  auto created = pp::amp::AmpStack::Create(io_, clock_, std::move(stack_config));
  if (!created) {
    return created.error();
  }
  stack_ = std::move(*created);
  stack_->Start();

  auto ma = pp::amp::FormatAdpMultiaddr(io_->LocalEndpoint(), stack_->LocalPeerId());
  if (!ma) {
    Stop();
    return ma.error();
  }
  listen_multiaddr_ = *ma;
  stack_->Links().SetLocalListenMultiaddrs({listen_multiaddr_});

  running_ = true;
  stop_ = false;
  pump_thread_ = std::thread([this]() { PumpLoop(); });
  return {};
}

void LedgerAmpRuntime::Stop() {
  stop_ = true;
  if (pump_thread_.joinable()) {
    pump_thread_.join();
  }
  if (stack_) {
    stack_->Stop();
    stack_.reset();
  }
  io_.reset();
  clock_.reset();
  running_ = false;
  listen_multiaddr_.clear();
}

pp::amp::PeerLinkManager& LedgerAmpRuntime::links() { return stack_->Links(); }

pp::amp::MeshRuntime& LedgerAmpRuntime::runtime() { return stack_->Runtime(); }

LedgerAmpRuntime::IoPump LedgerAmpRuntime::ioPump() const {
  return [this]() {
    if (stack_) {
      stack_->Pump();
      stack_->Tick();
    }
  };
}

void LedgerAmpRuntime::PumpLoop() {
  while (!stop_.load()) {
    if (stack_) {
      stack_->Pump();
      stack_->Tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

} // namespace network
} // namespace pp
