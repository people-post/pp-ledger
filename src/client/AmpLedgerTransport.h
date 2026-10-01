#pragma once

#include "ILedgerTransport.h"
#include "amp/link/MeshRuntime.h"
#include "common/Module.h"

#include <functional>
#include <string>

namespace pp {

namespace network {
class LedgerAmpRuntime;
}

/**
 * AMP channel transport for ledger RPC (unframed binaryPack on L3 DATA).
 *
 * All link and channel work runs on the AMP io lane; the calling thread never
 * drives the stack. Two modes:
 * - Threaded: a LedgerAmpRuntime pump thread drives. roundTrip() posts the call
 *   and blocks on its result. Must not be called from the pump thread.
 * - Caller-driven: no driver thread (tests / single-threaded tools). roundTrip()
 *   posts the call, then runs `drive` on the calling thread until it completes,
 *   so the caller is the only driver.
 */
class AmpLedgerTransport : public ILedgerTransport, public Module {
public:
  using DriveFn = std::function<void()>;

  AmpLedgerTransport(network::LedgerAmpRuntime& runtime, std::string peer_key);
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

  pp::amp::MeshRuntime& mesh_;
  network::LedgerAmpRuntime* runtime_{nullptr};
  DriveFn drive_;
  std::string peer_key_;
};

} // namespace pp
