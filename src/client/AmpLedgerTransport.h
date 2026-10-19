#pragma once

#include "ILedgerTransport.h"
#include "../network/InFlightLimit.h"
#include "amp/link/MeshRuntime.h"
#include "common/Module.h"

#include <functional>
#include <string>

namespace pp {

namespace network {
class AmpHost;
}

/**
 * AMP channel transport for ledger RPC (unframed binaryPack on L3 DATA).
 *
 * All link and channel work runs on the AMP io lane; the calling thread never
 * drives the stack. Two modes:
 * - Threaded: an AmpHost's driver thread drives — a LedgerAmpRuntime's pump, or
 *   a host's (HostedAmpRuntime). roundTrip() posts the call and blocks on its
 *   result. Must not be called from the driver thread. An async call's `done`
 *   runs only while the AmpHost is open.
 * - Caller-driven: no driver thread (tests / single-threaded tools). roundTrip()
 *   posts the call, then runs `drive` on the calling thread until it completes,
 *   so the caller is the only driver.
 */
class AmpLedgerTransport : public ILedgerTransport, public Module {
public:
  using DriveFn = std::function<void()>;

  AmpLedgerTransport(network::AmpHost& runtime, std::string peer_key);
  AmpLedgerTransport(pp::amp::MeshRuntime& mesh, std::string peer_key, DriveFn drive);

  void setPeerKey(std::string peer_key) { peer_key_ = std::move(peer_key); }
  const std::string& peerKey() const { return peer_key_; }

  /** RegisterEndpoint on the io lane. */
  bool registerEndpoint(const std::string& peer_key, const std::string& multiaddr);

  Roe<std::string> roundTrip(const std::string& requestBody, std::chrono::milliseconds timeout) override;

  /** `done` runs on the io lane. Safe to call from the pump thread. */
  void roundTripAsync(const std::string& requestBody, std::chrono::milliseconds timeout, Done done) override;

private:
  /** Run `task` on the io lane (posts; wakes the pump thread in threaded mode). */
  void post(std::function<void()> task);
  /**
   * The runtime's in-flight limit for requests of `requestBytes` (small and large ones are counted
   * apart, as the server does; shared by the runtime's clients), or this transport's own.
   */
  std::shared_ptr<network::InFlightLimit> inFlightLimit(size_t requestBytes) const;

  pp::amp::MeshRuntime& mesh_;
  network::AmpHost* runtime_{nullptr};
  DriveFn drive_;
  std::string peer_key_;
  std::shared_ptr<network::InFlightLimit> own_limit_;
  std::shared_ptr<network::InFlightLimit> own_bulk_limit_;
};

} // namespace pp
