#include "AmpLedgerTransport.h"

#include "../network/LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

#include <chrono>

namespace pp {
namespace {

using pp::ledger::rpc::kProtocolId;
using pp::ledger::rpc::LedgerRpcChannelPolicy;

void Pump(const AmpLedgerTransport::IoPump& io_pump) {
  if (io_pump) {
    io_pump();
  }
}

bool PastDeadline(const std::chrono::steady_clock::time_point deadline) {
  return std::chrono::steady_clock::now() >= deadline;
}

} // namespace

AmpLedgerTransport::AmpLedgerTransport(pp::amp::PeerLinkManager& links, std::string peer_key, IoPump io_pump)
    : links_(links), peer_key_(std::move(peer_key)), io_pump_(std::move(io_pump)) {}

AmpLedgerTransport::Roe<std::string> AmpLedgerTransport::roundTrip(const std::string& requestBody,
                                                                   const std::chrono::milliseconds timeout) {
  if (peer_key_.empty()) {
    return LedgerTransportError(-1, "AmpLedgerTransport: peer_key not set");
  }
  if (requestBody.size() > pp::ledger::rpc::kMaxPayloadBytes) {
    return LedgerTransportError(-1, "AmpLedgerTransport: request too large");
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;

  bool assoc_done = false;
  bool assoc_ok = false;
  std::string assoc_error;
  links_.EnsureAssociation(peer_key_, [&](pp::amp::PeerLinkManager::LinkRoe result) {
    assoc_done = true;
    if (result) {
      assoc_ok = true;
    } else {
      assoc_error = result.error().message;
    }
  });
  while (!assoc_done && !PastDeadline(deadline)) {
    Pump(io_pump_);
  }
  if (!assoc_done) {
    return LedgerTransportError(-1, "AmpLedgerTransport: association timeout");
  }
  if (!assoc_ok) {
    return LedgerTransportError(-1, assoc_error.empty() ? "AmpLedgerTransport: association failed" : assoc_error);
  }
  if (!links_.IsConnected(peer_key_)) {
    return LedgerTransportError(-1, "AmpLedgerTransport: association not connected");
  }

  bool channel_done = false;
  bool channel_ok = false;
  uint32_t channel_id = 0;
  std::string channel_error;
  links_.OpenChannel(peer_key_, kProtocolId, LedgerRpcChannelPolicy(), [&](pp::amp::PeerLinkManager::ChannelRoe ch) {
    channel_done = true;
    if (ch) {
      channel_ok = true;
      channel_id = ch.value();
    } else {
      channel_error = ch.error().message;
    }
  });
  while (!channel_done && !PastDeadline(deadline)) {
    Pump(io_pump_);
  }
  if (!channel_done) {
    return LedgerTransportError(-1, "AmpLedgerTransport: channel open timeout");
  }
  if (!channel_ok) {
    return LedgerTransportError(-1, channel_error.empty() ? "AmpLedgerTransport: channel open failed" : channel_error);
  }

  auto* link = links_.FindLink(peer_key_);
  if (!link || !link->Mux()) {
    return LedgerTransportError(-1, "AmpLedgerTransport: link unavailable");
  }

  while (link->Mux()->State(channel_id) != pp::amp::ChannelState::Open && !PastDeadline(deadline)) {
    Pump(io_pump_);
  }
  if (link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
    return LedgerTransportError(-1, "AmpLedgerTransport: channel not open");
  }

  bool response_done = false;
  std::string response_body;
  std::string frame_error;

  auto session = std::make_shared<pp::amp::ChannelSession>();
  session->Bind(*link->Mux(), channel_id, LedgerRpcChannelPolicy(),
                [&](pp::Roe<std::vector<uint8_t>> body) {
                  if (!body) {
                    frame_error = body.error().message;
                    response_done = true;
                    return false;
                  }
                  response_body.assign(body->begin(), body->end());
                  response_done = true;
                  return true;
                });

  std::vector<uint8_t> request(requestBody.begin(), requestBody.end());
  if (!session->EnqueueOutbound(std::move(request))) {
    return LedgerTransportError(-1, "AmpLedgerTransport: failed to enqueue request");
  }
  Pump(io_pump_);

  while (!response_done && !PastDeadline(deadline)) {
    Pump(io_pump_);
  }
  if (!response_done) {
    return LedgerTransportError(-1, "AmpLedgerTransport: response timeout");
  }
  if (!frame_error.empty()) {
    return LedgerTransportError(-1, frame_error);
  }
  if (response_body.size() > pp::ledger::rpc::kMaxPayloadBytes) {
    return LedgerTransportError(-1, "AmpLedgerTransport: response too large");
  }
  return response_body;
}

} // namespace pp
