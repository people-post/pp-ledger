#include "ledger/network/LedgerAmpRuntime.h"
#include "ledger/network/LedgerRpcProtocol.h"

#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/link/AdpMultiaddr.h"

namespace pp {
namespace network {

LedgerAmpRuntime::~LedgerAmpRuntime() { Stop(); }

pp::Roe<void> LedgerAmpRuntime::Start(LedgerAmpConfig config) {
  auto clock = std::make_shared<pp::adp::WallClock>();
  const auto local = pp::adp::IpEndpoint::V4(0, 0, 0, 0, config.udp_port);
  const pp::adp::UdpSocketOptions options{config.udp_buffer_bytes, config.udp_buffer_bytes};
  auto bound = pp::adp::OsUdpDatagramIo::Bind(local, options);
  if (!bound) {
    return bound.error();
  }
  std::shared_ptr<pp::adp::DatagramIo> io(std::move(*bound));
  const auto stats = io->Stats();
  buffers_capped_ = stats && (stats->receive_buffer_bytes < options.receive_buffer_bytes ||
                              stats->send_buffer_bytes < options.send_buffer_bytes);
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
  // A channel for a protocol this node does not serve is refused at the open, not left to time out
  // (e.g. a newer client's protocol on an older node).
  stack_->Links().SetRefuseUnhandledOpens(true);

  auto ma = pp::amp::FormatAdpMultiaddr(io_->LocalEndpoint(), stack_->LocalPeerId());
  if (!ma) {
    Stop();
    return ma.error();
  }
  listen_multiaddr_ = *ma;
  stack_->Links().SetLocalListenMultiaddrs({listen_multiaddr_});

  {
    std::lock_guard<std::mutex> lock(post_mu_);
    lane_ = stack_->Runtime().IoExecutor();
  }
  open();
  running_ = true;
  stop_ = false;
  pump_thread_ = std::thread([this]() { PumpLoop(); });
  return {};
}

void LedgerAmpRuntime::Stop() {
  close(); // no pp-ledger code runs from the io lane from here on
  stop_ = true;
  Wake();
  if (pump_thread_.joinable()) {
    pump_thread_.join();
  }
  std::unique_ptr<pp::amp::AmpStack> stack;
  {
    // After this, post() is a no-op. A post already holding the lane is safe: the lane outlives the
    // stack (its calls are dropped once the runtime goes). Tear down outside post_mu_.
    std::lock_guard<std::mutex> lock(post_mu_);
    running_ = false;
    lane_.reset();
    stack = std::move(stack_);
  }
  if (stack) {
    stack->Stop();
    stack.reset();
  }
  io_.reset();
  clock_.reset();
  listen_multiaddr_.clear();
}

std::optional<pp::adp::DatagramIoStats> LedgerAmpRuntime::socketStats() const {
  std::shared_ptr<pp::adp::DatagramIo> io;
  {
    std::lock_guard<std::mutex> lock(post_mu_); // Stop() drops io_ after running_ goes false under it
    if (!running_) {
      return std::nullopt;
    }
    io = io_;
  }
  return io ? io->Stats() : std::nullopt;
}

pp::amp::MeshRuntime& LedgerAmpRuntime::runtime() { return stack_->Runtime(); }

void LedgerAmpRuntime::post(IoTask task) {
  std::shared_ptr<pp::SerialExecutor> lane;
  {
    std::lock_guard<std::mutex> lock(post_mu_);
    if (!running_) {
      return;
    }
    lane = lane_;
  }
  // No lock held: posting takes amp's io lock, which the pump thread already holds when it posts
  // from inside Drive().
  lane->Post(guardTask(std::move(task)));
  Wake();
}

bool LedgerAmpRuntime::onIoThread() const {
  std::shared_ptr<pp::SerialExecutor> lane;
  {
    std::lock_guard<std::mutex> lock(post_mu_);
    lane = lane_;
  }
  return lane && lane->RunsTasksOnCurrentThread();
}

void LedgerAmpRuntime::Wake() {
  {
    std::lock_guard<std::mutex> lock(wake_mu_);
    wake_ = true;
  }
  wake_cv_.notify_one();
}

void LedgerAmpRuntime::PumpLoop() {
  // The pump is the stack's only driver: a drive from any other thread is refused (and counted).
  stack_->Runtime().ClaimDriver();
  auto& endpoint = stack_->GetEndpoint();
  while (!stop_.load()) {
    const uint64_t received = endpoint.Stats().rx_datagrams;
    stack_->Runtime().Drive();
    // A turn reads at most a budget of datagrams; one that read a whole budget left more waiting, so
    // go again at once. Waiting here capped a node at a budget per 5 ms whatever its CPU, and under a
    // burst of large requests its socket overflowed and dropped every peer's datagrams (lr8, M14).
    if (endpoint.Stats().rx_datagrams - received >= pp::adp::kDefaultPumpBudget) {
      continue;
    }
    // Idle cadence for timers/retransmits; post() wakes us early for new work.
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() { return wake_ || stop_.load(); });
    wake_ = false;
  }
  stack_->Runtime().ReleaseDriver();
}

} // namespace network
} // namespace pp
