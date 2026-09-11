#include "AmpLedgerServer.h"

#include "LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

namespace pp {
namespace network {
namespace {

using pp::ledger::rpc::kProtocolId;
using pp::ledger::rpc::LedgerRpcChannelPolicy;

void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id, AmpLedgerServer::Handler handler,
                          AmpLedgerServer::WorkerPost post_worker, AmpLedgerServer::IoPost post_io) {
  if (!link.Mux()) {
    return;
  }
  auto session = std::make_shared<pp::amp::ChannelSession>();
  // Server closes after the reply is enqueued. read_once would Close() immediately after the
  // request frame handler returns — before an async worker can EnqueueOutbound.
  auto policy = LedgerRpcChannelPolicy();
  policy.read_once = false;
  session->Bind(
      *link.Mux(), channel_id, std::move(policy),
      [session, handler = std::move(handler), post_worker = std::move(post_worker),
       post_io = std::move(post_io)](pp::Roe<std::vector<uint8_t>> body) {
        if (!body) {
          return false;
        }
        std::string request(body->begin(), body->end());
        auto finish = [session, handler, request = std::move(request), post_io]() {
          const std::string response = handler(request);
          auto send = [session, response]() {
            std::vector<uint8_t> out(response.begin(), response.end());
            (void)session->EnqueueOutbound(std::move(out));
            if (!session->IsClosed()) {
              session->CloseQuiet();
            }
          };
          if (post_io) {
            post_io(std::move(send));
          } else {
            send();
          }
        };
        if (post_worker) {
          post_worker(std::move(finish));
        } else {
          finish();
        }
        return true;
      });
}

} // namespace

void AmpLedgerServer::Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker,
                           IoPost post_io) {
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

  links.SetProtocolHandler(kProtocolId, [handler = std::move(handler), post_worker = std::move(post_worker),
                                         post_io = std::move(post_io)](pp::amp::PeerLink& link,
                                                                      const uint32_t channel_id) {
    // Copy handler/post_* per channel — moving would break the second inbound RPC.
    HandleInboundChannel(link, channel_id, handler, post_worker, post_io);
  });
}

void AmpLedgerServer::Unbind(pp::amp::PeerLinkManager& links) { links.RemoveProtocolHandler(kProtocolId); }

} // namespace network
} // namespace pp
