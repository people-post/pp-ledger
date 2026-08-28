#include "TcpLedgerTransport.h"

namespace pp {

TcpLedgerTransport::TcpLedgerTransport() {
  fetchClient_.redirectLogger(log().getFullName() + ".FetchClient");
}

void TcpLedgerTransport::setEndpoint(const network::IpEndpoint &endpoint) {
  endpoint_ = endpoint;
}

TcpLedgerTransport::Roe<std::string>
TcpLedgerTransport::roundTrip(const std::string &requestBody,
                              std::chrono::milliseconds timeout) {
  if (endpoint_.port == 0) {
    return LedgerTransportError(1, "No remote endpoint configured");
  }

  auto result = fetchClient_.fetchSync(endpoint_, requestBody, timeout);
  if (!result.isOk()) {
    return LedgerTransportError(result.error().code, result.error().message);
  }
  return result.value();
}

} // namespace pp
