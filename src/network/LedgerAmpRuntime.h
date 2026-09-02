#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/DatagramIo.h"
#include "amp/L2/Types.h"
#include "amp/link/AmpStack.h"
#include "amp/link/MeshRuntime.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace pp {
namespace network {

/** Slim AMP runtime for standalone pp-ledger servers (no product L4). */
struct LedgerAmpConfig {
  pp::amp::MshIdentity identity;
  std::string local_peer_id;
  pp::amp::PeerLinkConfig link_config{};
  uint16_t udp_port = 8519;
};

class LedgerAmpRuntime {
public:
  using IoPump = std::function<void()>;

  LedgerAmpRuntime() = default;
  ~LedgerAmpRuntime();

  LedgerAmpRuntime(const LedgerAmpRuntime&) = delete;
  LedgerAmpRuntime& operator=(const LedgerAmpRuntime&) = delete;

  /** UDP listen on config.udp_port (OsUdpDatagramIo). */
  pp::Roe<void> Start(LedgerAmpConfig config);

  /** In-memory datagram IO for unit tests (no UDP). */
  pp::Roe<void> StartForTest(std::shared_ptr<pp::adp::DatagramIo> io, std::shared_ptr<pp::adp::Clock> clock,
                             LedgerAmpConfig config);

  void Stop();

  bool isRunning() const { return running_; }

  pp::amp::PeerLinkManager& links();
  pp::amp::MeshRuntime& runtime();
  IoPump ioPump() const;

  std::string listenMultiaddr() const { return listen_multiaddr_; }

private:
  void PumpLoop();

  std::shared_ptr<pp::adp::DatagramIo> io_;
  std::shared_ptr<pp::adp::Clock> clock_;
  std::unique_ptr<pp::amp::AmpStack> stack_;
  std::string listen_multiaddr_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread pump_thread_;
};

} // namespace network
} // namespace pp
