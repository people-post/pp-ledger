#include "AmpLedgerTransport.h"

#include "../network/LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

#include <chrono>
#include <memory>

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

/** Unbind safely: if the PeerLink/Mux was torn down during pump, do not touch dangling mux_. */
void DetachSession(pp::amp::PeerLinkManager& links, const std::string& peer_key,
                   pp::amp::ChannelMux* bound_mux, const std::shared_ptr<pp::amp::ChannelSession>& session) {
  if (!session) {
    return;
  }
  auto* live = links.FindLink(peer_key);
  if (bound_mux && live && live->Mux() == bound_mux) {
    session->ReleaseHandlers();
  } else {
    session->OrphanFromMux();
  }
}

} // namespace

AmpLedgerTransport::AmpLedgerTransport(pp::amp::PeerLinkManager& links, std::string peer_key, IoPump io_pump,
                                       IoExclusive io_exclusive)
    : links_(links), peer_key_(std::move(peer_key)), io_pump_(std::move(io_pump)),
      io_exclusive_(std::move(io_exclusive)) {}

void AmpLedgerTransport::exclusive(const std::function<void()>& fn) const {
  if (io_exclusive_) {
    io_exclusive_(fn);
  } else {
    fn();
  }
}

bool AmpLedgerTransport::registerEndpoint(const std::string& peer_key, const std::string& multiaddr) {
  bool ok = false;
  exclusive([&]() { ok = links_.RegisterEndpoint(peer_key, multiaddr).isOk(); });
  return ok;
}

AmpLedgerTransport::Roe<std::string> AmpLedgerTransport::roundTrip(const std::string& requestBody,
                                                                   const std::chrono::milliseconds timeout) {
  if (peer_key_.empty()) {
    return LedgerTransportError(-1, "AmpLedgerTransport: peer_key not set");
  }
  if (requestBody.size() > pp::ledger::rpc::kMaxPayloadBytes) {
    return LedgerTransportError(-1, "AmpLedgerTransport: request too large");
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const std::string peer_key = peer_key_;

  // Link callbacks may fire after a timeout return; they only touch this shared state.
  struct State {
    bool assoc_done = false;
    bool assoc_ok = false;
    std::string assoc_error;
    bool channel_done = false;
    bool channel_ok = false;
    uint32_t channel_id = 0;
    std::string channel_error;
    bool response_done = false;
    std::string response_body;
    std::string frame_error;
  };
  auto st = std::make_shared<State>();

  auto wait_until = [&](const std::function<bool()>& ready) {
    while (true) {
      bool done = false;
      exclusive([&]() { done = ready(); });
      if (done || PastDeadline(deadline)) {
        return done;
      }
      Pump(io_pump_);
    }
  };

  exclusive([&]() {
    links_.EnsureAssociation(peer_key, [st](pp::amp::PeerLinkManager::LinkRoe result) {
      st->assoc_done = true;
      if (result) {
        st->assoc_ok = true;
      } else {
        st->assoc_error = result.error().message;
      }
    });
  });
  if (!wait_until([&]() { return st->assoc_done; })) {
    return LedgerTransportError(-1, "AmpLedgerTransport: association timeout");
  }
  bool assoc_ok = false;
  bool connected = false;
  std::string assoc_error;
  exclusive([&]() {
    assoc_ok = st->assoc_ok;
    assoc_error = st->assoc_error;
    connected = assoc_ok && links_.IsConnected(peer_key);
  });
  if (!assoc_ok) {
    return LedgerTransportError(-1, assoc_error.empty() ? "AmpLedgerTransport: association failed" : assoc_error);
  }
  if (!connected) {
    return LedgerTransportError(-1, "AmpLedgerTransport: association not connected");
  }

  exclusive([&]() {
    links_.OpenChannel(peer_key, kProtocolId, LedgerRpcChannelPolicy(), [st](pp::amp::PeerLinkManager::ChannelRoe ch) {
      st->channel_done = true;
      if (ch) {
        st->channel_ok = true;
        st->channel_id = ch.value();
      } else {
        st->channel_error = ch.error().message;
      }
    });
  });
  if (!wait_until([&]() { return st->channel_done; })) {
    return LedgerTransportError(-1, "AmpLedgerTransport: channel open timeout");
  }
  bool channel_ok = false;
  uint32_t channel_id = 0;
  std::string channel_error;
  exclusive([&]() {
    channel_ok = st->channel_ok;
    channel_id = st->channel_id;
    channel_error = st->channel_error;
  });
  if (!channel_ok) {
    return LedgerTransportError(-1, channel_error.empty() ? "AmpLedgerTransport: channel open failed" : channel_error);
  }

  bool link_ok = false;
  exclusive([&]() {
    auto* link = links_.FindLink(peer_key);
    link_ok = link && link->Mux();
  });
  if (!link_ok) {
    return LedgerTransportError(-1, "AmpLedgerTransport: link unavailable");
  }

  // Re-resolve the mux on every check: the link may be torn down between pumps.
  auto channel_open = [&]() {
    auto* link = links_.FindLink(peer_key);
    return link && link->Mux() && link->Mux()->State(channel_id) == pp::amp::ChannelState::Open;
  };
  if (!wait_until(channel_open)) {
    return LedgerTransportError(-1, "AmpLedgerTransport: channel not open");
  }

  auto session = std::make_shared<pp::amp::ChannelSession>();
  pp::amp::ChannelMux* bound_mux = nullptr;
  bool enqueued = false;
  exclusive([&]() {
    auto* link = links_.FindLink(peer_key);
    if (!link || !link->Mux()) {
      return;
    }
    bound_mux = link->Mux();
    session->Bind(*bound_mux, channel_id, LedgerRpcChannelPolicy(), [st](pp::Roe<std::vector<uint8_t>> body) {
      if (!body) {
        st->frame_error = body.error().message;
        st->response_done = true;
        return false;
      }
      st->response_body.assign(body->begin(), body->end());
      st->response_done = true;
      return true;
    });
    std::vector<uint8_t> request(requestBody.begin(), requestBody.end());
    enqueued = session->EnqueueOutbound(std::move(request));
    if (!enqueued) {
      DetachSession(links_, peer_key, bound_mux, session);
    }
  });
  if (!bound_mux) {
    return LedgerTransportError(-1, "AmpLedgerTransport: link unavailable");
  }
  if (!enqueued) {
    return LedgerTransportError(-1, "AmpLedgerTransport: failed to enqueue request");
  }
  Pump(io_pump_);

  const bool response_done = wait_until([&]() { return st->response_done; });
  std::string response_body;
  std::string frame_error;
  exclusive([&]() {
    DetachSession(links_, peer_key, bound_mux, session);
    response_body = std::move(st->response_body);
    frame_error = st->frame_error;
  });
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
