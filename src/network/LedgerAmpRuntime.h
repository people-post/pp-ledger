#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/DatagramIo.h"
#include "amp/L2/Types.h"
#include "amp/link/AmpStack.h"
#include "amp/link/MeshRuntime.h"
#include "InFlightLimit.h"
#include "NetworkTuning.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
  /** The UDP socket's receive and send buffers asked for; 0 = the OS default (clients). */
  size_t udp_buffer_bytes = 0;
  /** Servers: read timeout of inbound RPC channels (NetworkTuning::channelReadTimeout). */
  std::chrono::milliseconds rpc_read_timeout{NetworkTuning::dataTimeoutFor(NetworkTuning::kDefaultRpcTimeout)};
};

/**
 * Owns the AMP stack and its single driver thread (the pump thread).
 *
 * Only the pump thread drives the stack (Drive/Pump/Tick). Other threads never
 * touch links or channel sessions directly: they post() work onto the io lane
 * and wait for its result (see AmpLedgerTransport). See
 * docs/architecture/THREADING.md.
 */
class LedgerAmpRuntime {
public:
  using IoTask = std::function<void()>;

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

  /**
   * Setup-time access (bind handlers, advertise protocols) before traffic flows.
   * Do not use for per-request link or channel work — post() it instead.
   */
  pp::amp::PeerLinkManager& links();
  pp::amp::MeshRuntime& runtime();

  /** Run `task` on the pump thread's io lane. Any thread; no-op when stopped. */
  void post(IoTask task);

  /** True on the pump thread. Blocking on io work from here would deadlock. */
  bool onPumpThread() const;

  std::string listenMultiaddr() const { return listen_multiaddr_; }

  /** The socket's buffers as granted and the datagrams the OS dropped for want of room; none when stopped or in tests. */
  std::optional<pp::adp::DatagramIoStats> socketStats() const;
  /** Start: the buffers asked for were cut short by the OS's cap (e.g. `net.core.rmem_max`). */
  bool socketBuffersCapped() const { return buffers_capped_; }

  /**
   * io lane: requests in flight to each peer from every client on this runtime (InFlightLimit);
   * a fresh one at each Start, so a stop leaves no count behind.
   */
  const std::shared_ptr<InFlightLimit> &inFlightLimit() const { return in_flight_; }
  /** The same for large requests (kBulkProtocolId), which servers take fewer of at once. */
  const std::shared_ptr<InFlightLimit> &bulkInFlightLimit() const { return bulk_in_flight_; }

private:
  void PumpLoop();
  void Wake();

  std::shared_ptr<pp::adp::DatagramIo> io_;
  std::shared_ptr<pp::adp::Clock> clock_;
  std::unique_ptr<pp::amp::AmpStack> stack_;
  std::string listen_multiaddr_;
  std::atomic<bool> buffers_capped_{false};
  std::shared_ptr<InFlightLimit> in_flight_;
  std::shared_ptr<InFlightLimit> bulk_in_flight_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread pump_thread_;
  std::atomic<std::thread::id> pump_thread_id_{};
  /**
   * Gates post() against Stop() tearing down stack_. Never held while calling
   * into the stack: the pump thread posts while holding amp's io lock.
   */
  mutable std::mutex post_mu_;
  std::condition_variable posts_drained_cv_;
  size_t posts_in_flight_{0};
  std::mutex wake_mu_;
  std::condition_variable wake_cv_;
  bool wake_{false};
};

} // namespace network
} // namespace pp
