#include "AmpLedgerServer.h"

#include "LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

namespace pp {
namespace network {
namespace {

using pp::ledger::rpc::kProtocolId;
using pp::ledger::rpc::LedgerRpcChannelPolicy;

void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id, AmpLedgerServer::Handler handler,
                          AmpLedgerServer::WorkerPost post_worker) {
  if (!link.Mux()) {
    return;
  }
  auto session = std::make_shared<pp::amp::ChannelSession>();
  session->Bind(
      *link.Mux(), channel_id, LedgerRpcChannelPolicy(),
      [session, handler = std::move(handler), post_worker = std::move(post_worker)](
          pp::Roe<std::vector<uint8_t>> body) {
        if (!body) {
          return false;
        }
        std::string request(body->begin(), body->end());
        auto respond = [session, handler, request = std::move(request)]() {
          const std::string response = handler(request);
          std::vector<uint8_t> out(response.begin(), response.end());
          session->EnqueueOutbound(std::move(out));
        };
        if (post_worker) {
          post_worker(std::move(respond));
        } else {
          respond();
        }
        return true;
      });
}

} // namespace

void AmpLedgerServer::Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker) {
  auto protocols = links.LocalCapability().protocols;
  bool found = false;
  for (const auto& id : protocols) {
    if (id == kProtocolId) {
      found = true;
      break;
    }
  }
  if (!found) {
    protocols.push_back(kProtocolId);
    links.SetAdvertisedProtocols(std::move(protocols));
  }

  links.SetProtocolHandler(kProtocolId, [handler = std::move(handler),
                                         post_worker = std::move(post_worker)](pp::amp::PeerLink& link,
                                                                               const uint32_t channel_id) mutable {
    HandleInboundChannel(link, channel_id, std::move(handler), std::move(post_worker));
  });
}

void AmpLedgerServer::Unbind(pp::amp::PeerLinkManager& links) { links.RemoveProtocolHandler(kProtocolId); }

} // namespace network
} // namespace pp
