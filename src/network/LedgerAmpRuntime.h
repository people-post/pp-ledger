#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/DatagramIo.h"
#include "amp/L2/Types.h"
#include "amp/link/AmpStack.h"
#include "amp/link/MeshRuntime.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
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
  /** Runs a callable while holding exclusive access to the AMP stack. */
  using IoExclusive = std::function<void(const std::function<void()>&)>;

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

  /** Callers on other threads must use runExclusive() around link/mux access. */
  pp::amp::PeerLinkManager& links();
  pp::amp::MeshRuntime& runtime();
  /**
   * Pump/Tick run only on the pump thread. Off that thread the returned callable
   * wakes the pump thread and yields instead of driving the stack.
   */
  IoPump ioPump() const;
  IoExclusive ioExclusive() const;
  void runExclusive(const std::function<void()>& fn) const;

  std::string listenMultiaddr() const { return listen_multiaddr_; }

private:
  void PumpLoop();
  void Wake() const;

  std::shared_ptr<pp::adp::DatagramIo> io_;
  std::shared_ptr<pp::adp::Clock> clock_;
  std::unique_ptr<pp::amp::AmpStack> stack_;
  std::string listen_multiaddr_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread pump_thread_;
  std::atomic<std::thread::id> pump_thread_id_{};
  /** Held for every Pump/Tick and for off-thread link access. */
  mutable std::recursive_mutex stack_mu_;
  mutable std::mutex wake_mu_;
  mutable std::condition_variable wake_cv_;
  mutable bool wake_{false};
};

} // namespace network
} // namespace pp
