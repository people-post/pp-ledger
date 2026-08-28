#pragma once

#include "common/ResultOrError.hpp"

#include <chrono>
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
};

} // namespace pp
