#pragma once

#include "ILedgerTransport.h"

#include <functional>
#include <string>

namespace pp {

using UnframedRequestHandler =
    std::function<std::string(const std::string &requestBody)>;

/** Same-process transport: no length prefix; dispatches unframed RPC bytes. */
class InProcessLedgerTransport : public ILedgerTransport {
public:
  explicit InProcessLedgerTransport(UnframedRequestHandler handler);

  bool requiresRemoteEndpoint() const override { return false; }

  Roe<std::string> roundTrip(const std::string &requestBody,
                             std::chrono::milliseconds timeout) override;

private:
  UnframedRequestHandler handler_;
};

} // namespace pp
