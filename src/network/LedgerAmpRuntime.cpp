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
  // Amp UDP accept is required for inbound dials (beacon/relay/miner peers and client RPC).
  // PeerLinkManager installs accept key + handler but leaves accept_enabled_ false by default.
  stack_->GetEndpoint().SetAcceptEnabled(true);

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
  Wake();
  if (pump_thread_.joinable()) {
    pump_thread_.join();
  }
  pump_thread_id_ = std::thread::id{};
  running_ = false;
  std::lock_guard<std::recursive_mutex> lock(stack_mu_);
  if (stack_) {
    stack_->Stop();
    stack_.reset();
  }
  io_.reset();
  clock_.reset();
  listen_multiaddr_.clear();
}

pp::amp::PeerLinkManager& LedgerAmpRuntime::links() { return stack_->Links(); }

pp::amp::MeshRuntime& LedgerAmpRuntime::runtime() { return stack_->Runtime(); }

LedgerAmpRuntime::IoPump LedgerAmpRuntime::ioPump() const {
  return [this]() {
    if (running_.load() && std::this_thread::get_id() != pump_thread_id_.load()) {
      Wake();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      return;
    }
    std::lock_guard<std::recursive_mutex> lock(stack_mu_);
    if (stack_) {
      stack_->Pump();
      stack_->Tick();
    }
  };
}

LedgerAmpRuntime::IoExclusive LedgerAmpRuntime::ioExclusive() const {
  return [this](const std::function<void()>& fn) { runExclusive(fn); };
}

void LedgerAmpRuntime::runExclusive(const std::function<void()>& fn) const {
  std::lock_guard<std::recursive_mutex> lock(stack_mu_);
  fn();
}

void LedgerAmpRuntime::Wake() const {
  {
    std::lock_guard<std::mutex> lock(wake_mu_);
    wake_ = true;
  }
  wake_cv_.notify_one();
}

void LedgerAmpRuntime::PumpLoop() {
  pump_thread_id_ = std::this_thread::get_id();
  while (!stop_.load()) {
    {
      std::lock_guard<std::recursive_mutex> lock(stack_mu_);
      if (stack_) {
        stack_->Pump();
        stack_->Tick();
      }
    }
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() { return wake_ || stop_.load(); });
    wake_ = false;
  }
}

} // namespace network
} // namespace pp
