#include "AmpLedgerServer.h"

#include "LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

#include <atomic>
#include <memory>

namespace pp {
namespace network {
namespace {

using pp::ledger::rpc::kProtocolId;
using pp::ledger::rpc::LedgerRpcChannelPolicy;

void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id,
                          const AmpLedgerServer::AsyncHandler& handler, const AmpLedgerServer::IoPost& post_io) {
  if (!link.Mux()) {
    return;
  }
  auto session = std::make_shared<pp::amp::ChannelSession>();
  // Server closes after the reply is enqueued. read_once would Close() immediately after the
  // request frame handler returns — before a deferred reply can EnqueueOutbound.
  auto policy = LedgerRpcChannelPolicy();
  policy.read_once = false;
  session->Bind(*link.Mux(), channel_id, std::move(policy),
                [session, handler, post_io](pp::Roe<std::vector<uint8_t>> body) {
                  if (!body) {
                    return false;
                  }
                  auto sent = std::make_shared<std::atomic<bool>>(false);
                  AmpLedgerServer::Reply reply = [session, post_io, sent](std::string response) {
                    if (sent->exchange(true)) {
                      return;
                    }
                    // ChannelSession is io-thread affine: send on the io lane.
                    auto send = [session, response = std::move(response)]() {
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
                  handler(std::string(body->begin(), body->end()), std::move(reply));
                  return true;
                });
}

} // namespace

void AmpLedgerServer::BindAsync(pp::amp::PeerLinkManager& links, AsyncHandler handler, IoPost post_io) {
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

  links.SetProtocolHandler(kProtocolId, [&links, handler = std::move(handler), post_io = std::move(post_io)](
                                            pp::amp::LinkHandle link_handle, const std::string& /*remote_peer_id*/,
                                            const uint32_t channel_id) {
    links.WithLiveLink(link_handle,
                       [&](pp::amp::PeerLink& link) { HandleInboundChannel(link, channel_id, handler, post_io); });
  });
}

void AmpLedgerServer::Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker,
                           IoPost post_io) {
  BindAsync(
      links,
      [handler = std::move(handler), post_worker = std::move(post_worker)](std::string request, Reply reply) {
        auto run = [handler, request = std::move(request), reply = std::move(reply)]() { reply(handler(request)); };
        if (post_worker) {
          post_worker(std::move(run));
        } else {
          run();
        }
      },
      std::move(post_io));
}

void AmpLedgerServer::Unbind(pp::amp::PeerLinkManager& links) { links.RemoveProtocolHandler(kProtocolId); }

} // namespace network
} // namespace pp
