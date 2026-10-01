#pragma once

#include "common/ResultOrError.hpp"

#include <chrono>
#include <functional>
#include <string>

namespace pp {

struct LedgerTransportError : RoeErrorBase {
  using RoeErrorBase::RoeErrorBase;
};

/**
 * Pluggable transport for ledger RPC envelope bytes.
 *
 * roundTrip() carries unframed binaryPack(Client::Request) bytes in and returns
 * unframed binaryPack(Client::Response) bytes out. Stream transports (TCP,
 * libp2p) apply LedgerFrameCodec internally; in-process omits framing.
 */
class ILedgerTransport {
public:
  virtual ~ILedgerTransport() = default;

  template <typename T> using Roe = ResultOrError<T, LedgerTransportError>;

  virtual bool requiresRemoteEndpoint() const { return true; }

  virtual Roe<std::string> roundTrip(const std::string &requestBody,
                                     std::chrono::milliseconds timeout) = 0;

  using Done = std::function<void(Roe<std::string>)>;

  /**
   * Start a round trip and return; `done` runs exactly once with the result.
   * It may run on a transport thread (AMP: the io lane), so it must not touch
   * caller state directly. Default: blocking roundTrip(), `done` inline.
   */
  virtual void roundTripAsync(const std::string &requestBody,
                              std::chrono::milliseconds timeout, Done done) {
    done(roundTrip(requestBody, timeout));
  }
};

} // namespace pp
