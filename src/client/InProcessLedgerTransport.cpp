#include "InProcessLedgerTransport.h"

namespace pp {

InProcessLedgerTransport::InProcessLedgerTransport(UnframedRequestHandler handler)
    : handler_(std::move(handler)) {}

InProcessLedgerTransport::Roe<std::string>
InProcessLedgerTransport::roundTrip(const std::string &requestBody,
                                    std::chrono::milliseconds /*timeout*/) {
  if (!handler_) {
    return LedgerTransportError(1, "In-process handler not configured");
  }
  return handler_(requestBody);
}

} // namespace pp
