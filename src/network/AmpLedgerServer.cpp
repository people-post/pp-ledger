#include "AmpLedgerServer.h"

#include "LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace pp {
namespace network {
namespace {

using pp::ledger::rpc::kMaxReceivingPerPeer;
using pp::ledger::rpc::kProtocolId;
using pp::ledger::rpc::LedgerRpcChannelPolicy;

/** Channels each peer has receiving a request (B034): what a peer can make this node buffer. */
class Receiving {
public:
  /** Holds one of a peer's slots; gives it back when destroyed. */
  struct Slot {
    Slot(std::shared_ptr<Receiving> owner, std::string peer) : owner_(std::move(owner)), peer_(std::move(peer)) {}
    ~Slot() { owner_->release(peer_); }
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

  private:
    std::shared_ptr<Receiving> owner_;
    std::string peer_;
  };

  /** A slot for `peer`'s channel; none past the limit. */
  static std::shared_ptr<Slot> acquire(const std::shared_ptr<Receiving>& self, const std::string& peer) {
    {
      std::lock_guard<std::mutex> lock(self->mu_);
      auto& open = self->open_[peer];
      if (open >= kMaxReceivingPerPeer) {
        return nullptr;
      }
      ++open;
    }
    return std::make_shared<Slot>(self, peer);
  }

private:
  void release(const std::string& peer) {
    std::lock_guard<std::mutex> lock(mu_);
    if (auto it = open_.find(peer); it != open_.end() && --it->second == 0) {
      open_.erase(it);
    }
  }

  std::mutex mu_;
  std::map<std::string, size_t> open_;
};

void HandleInboundChannel(pp::amp::PeerLink& link, const std::string& remote_peer_id, const uint32_t channel_id,
                          const AmpLedgerServer::AsyncHandler& handler, const AmpLedgerServer::IoPost& post_io,
                          const std::chrono::milliseconds read_timeout, const std::shared_ptr<Receiving>& receiving) {
  if (!link.Mux()) {
    return;
  }
  auto session = std::make_shared<pp::amp::ChannelSession>();
  // The slot is held while the request arrives: released with the body, or when the channel closes
  // first (its handlers, holding the token, are dropped).
  auto slot = std::make_shared<std::shared_ptr<Receiving::Slot>>(Receiving::acquire(receiving, remote_peer_id));
  if (!*slot) {
    session->Bind(*link.Mux(), channel_id, LedgerRpcChannelPolicy(read_timeout), [](pp::Roe<std::vector<uint8_t>>) {
      return false;
    });
    session->Reset(); // too many requests arriving from this peer at once
    return;
  }
  // Server closes after the reply is enqueued. read_once would Close() immediately after the
  // request frame handler returns — before a deferred reply can EnqueueOutbound.
  auto policy = LedgerRpcChannelPolicy(read_timeout);
  policy.read_once = false;
  session->Bind(*link.Mux(), channel_id, std::move(policy),
                [session, handler, post_io, remote_peer_id, slot](pp::Roe<std::vector<uint8_t>> body) {
                  slot->reset(); // the body is complete (or failed): no longer receiving
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
                  handler(remote_peer_id, std::string(body->begin(), body->end()), std::move(reply));
                  return true;
                });
}

} // namespace

void AmpLedgerServer::BindAsync(pp::amp::PeerLinkManager& links, AsyncHandler handler, IoPost post_io,
                                const std::chrono::milliseconds read_timeout) {
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

  auto receiving = std::make_shared<Receiving>();
  links.SetProtocolHandler(kProtocolId, [&links, handler = std::move(handler), post_io = std::move(post_io), read_timeout,
                                         receiving](pp::amp::LinkHandle link_handle, const std::string& remote_peer_id,
                                                    const uint32_t channel_id) {
    links.WithLiveLink(link_handle, [&](pp::amp::PeerLink& link) {
      HandleInboundChannel(link, remote_peer_id, channel_id, handler, post_io, read_timeout, receiving);
    });
  });
}

void AmpLedgerServer::Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker,
                           IoPost post_io, const std::chrono::milliseconds read_timeout) {
  BindAsync(
      links,
      [handler = std::move(handler), post_worker = std::move(post_worker)](std::string /*remotePeerId*/,
                                                                           std::string request, Reply reply) {
        auto run = [handler, request = std::move(request), reply = std::move(reply)]() { reply(handler(request)); };
        if (post_worker) {
          post_worker(std::move(run));
        } else {
          run();
        }
      },
      std::move(post_io), read_timeout);
}

void AmpLedgerServer::Unbind(pp::amp::PeerLinkManager& links) { links.RemoveProtocolHandler(kProtocolId); }

} // namespace network
} // namespace pp
