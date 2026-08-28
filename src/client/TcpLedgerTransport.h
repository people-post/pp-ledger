#pragma once

#include "ILedgerTransport.h"
#include "lib/common/Module.h"
#include "../network/FetchClient.h"
#include "../network/Types.hpp"

namespace pp {

/** TCP fetch transport: applies u32 BE LedgerFrameCodec on the wire. */
class TcpLedgerTransport : public ILedgerTransport, public Module {
public:
  TcpLedgerTransport();

  void setEndpoint(const network::IpEndpoint &endpoint);
  const network::IpEndpoint &endpoint() const { return endpoint_; }

  bool requiresRemoteEndpoint() const override { return true; }

  Roe<std::string> roundTrip(const std::string &requestBody,
                             std::chrono::milliseconds timeout) override;

private:
  network::IpEndpoint endpoint_;
  network::FetchClient fetchClient_;
};

} // namespace pp
