#include "ledger/network/AmpLedgerServer.h"

#include "ledger/network/LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

namespace pp {
namespace network {
namespace {

using pp::ledger::rpc::kBulkProtocolId;
using pp::ledger::rpc::kMaxMessageBytes;
using pp::ledger::rpc::kMaxReceivingPerPeer;
using pp::ledger::rpc::kMaxSmallRequestBytes;
using pp::ledger::rpc::kProtocolId;
using pp::ledger::rpc::LedgerRpcChannelPolicy;
using Refusal = AmpLedgerServer::Refusal;

/**
 * Channels receiving a request (opened, body not yet complete), per peer and in all: what peers can
 * make this node buffer, and how much of its socket large bodies can take at once (B034, lr8).
 */
class Receiving {
public:
  Receiving(const size_t perPeer, const size_t total) : perPeer_(perPeer), total_(total) {}

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

  /** A slot for `peer`'s channel; none past either limit, with why in `refused`. */
  static std::shared_ptr<Slot> acquire(const std::shared_ptr<Receiving>& self, const std::string& peer,
                                       Refusal& refused) {
    {
      std::lock_guard<std::mutex> lock(self->mu_);
      auto& open = self->open_[peer];
      if (open >= self->perPeer_) {
        refused = Refusal::PeerBusy;
        return nullptr;
      }
      if (self->all_ >= self->total_) {
        if (open == 0) {
          self->open_.erase(peer);
        }
        refused = Refusal::NodeBusy;
        return nullptr;
      }
      ++open;
      ++self->all_;
    }
    return std::make_shared<Slot>(self, peer);
  }

private:
  void release(const std::string& peer) {
    std::lock_guard<std::mutex> lock(mu_);
    --all_;
    if (auto it = open_.find(peer); it != open_.end() && --it->second == 0) {
      open_.erase(it);
    }
  }

  const size_t perPeer_;
  const size_t total_;
  std::mutex mu_;
  std::map<std::string, size_t> open_;
  size_t all_{0};
};

/** One of the two RPC protocols: how large its requests come and how many may arrive at once. */
struct Lane {
  const char* protocolId;
  size_t maxRequestBytes;
  bool bulk;
  std::shared_ptr<Receiving> receiving;
  /** Trusted peers' requests, outside the node's share (bulk only; else null). */
  std::shared_ptr<Receiving> trustedReceiving;
};

pp::amp::ChannelPolicy InboundPolicy(const Lane& lane, const std::chrono::milliseconds read_timeout) {
  auto policy = LedgerRpcChannelPolicy(read_timeout);
  // The server closes after the reply is enqueued. read_once would Close() as soon as the request's
  // frame handler returns — before a deferred reply can EnqueueOutbound.
  policy.read_once = false;
  policy.max_message_bytes = lane.maxRequestBytes;
  return policy;
}

/** io lane: send `response` on `session` and close it; replies may be larger than requests were. */
void SendAndClose(pp::amp::ChannelSession& session, const std::chrono::milliseconds read_timeout,
                  std::string response) {
  if (session.IsClosed()) {
    return;
  }
  if (auto* mux = session.Mux()) {
    auto policy = LedgerRpcChannelPolicy(read_timeout);
    policy.read_once = false;
    (void)mux->ApplyChannelPolicy(session.ChannelId(), std::move(policy));
  }
  (void)session.EnqueueOutbound(std::vector<uint8_t>(response.begin(), response.end()));
  if (!session.IsClosed()) {
    session.CloseQuiet();
  }
}

void HandleInboundChannel(pp::amp::PeerLink& link, const std::string& remote_peer_id, const uint32_t channel_id,
                          const AmpLedgerServer::AsyncHandler& handler, const AmpLedgerServer::Options& options,
                          const Lane& lane) {
  if (!link.Mux()) {
    return;
  }
  const auto read_timeout = options.read_timeout;
  auto session = std::make_shared<pp::amp::ChannelSession>();
  // A refused request is answered at once, before its body is sent: the client binds its channel when
  // it opens and sends the body only once the open is acknowledged, so it reads this reply first.
  auto refuse = [&](const Refusal why) {
    session->Bind(*link.Mux(), channel_id, InboundPolicy(lane, read_timeout), [](pp::Roe<std::vector<uint8_t>>) {
      return false;
    });
    if (!options.refusal) {
      session->Reset();
      return;
    }
    SendAndClose(*session, read_timeout, options.refusal(why));
  };
  // Strangers are refused before they can send a large body (a small one is checked when it arrives:
  // HEALTH is served to anyone).
  if (lane.bulk && options.admit && !options.admit(remote_peer_id)) {
    refuse(Refusal::NotAllowed);
    return;
  }
  // The slot is held while the request arrives — a large one until it is answered, so large requests
  // waiting in the server's queue count too and cannot fill the queue small ones share. Released then,
  // or when the channel closes first (its handlers, holding the token, are dropped).
  auto refused = Refusal::PeerBusy;
  const auto& receiving =
      lane.trustedReceiving && options.trusted && options.trusted(remote_peer_id) ? lane.trustedReceiving : lane.receiving;
  auto slot = std::make_shared<std::shared_ptr<Receiving::Slot>>(Receiving::acquire(receiving, remote_peer_id, refused));
  if (!*slot) {
    refuse(refused);
    return;
  }
  const auto post_io = options.post_io;
  const bool holdUntilReply = lane.bulk;
  session->Bind(*link.Mux(), channel_id, InboundPolicy(lane, read_timeout),
                [session, handler, post_io, read_timeout, remote_peer_id, slot,
                 holdUntilReply](pp::Roe<std::vector<uint8_t>> body) {
                  // The body is complete (or failed): no longer receiving.
                  auto held = std::make_shared<std::shared_ptr<Receiving::Slot>>();
                  if (body && holdUntilReply) {
                    *held = std::move(*slot);
                  }
                  slot->reset();
                  if (!body) {
                    return false;
                  }
                  auto sent = std::make_shared<std::atomic<bool>>(false);
                  AmpLedgerServer::Reply reply = [session, post_io, read_timeout, sent, held](std::string response) {
                    if (sent->exchange(true)) {
                      return;
                    }
                    held->reset(); // answered (a reply never sent frees it with the last copy of `reply`)
                    // ChannelSession is io-thread affine: send on the io lane.
                    auto send = [session, read_timeout, response = std::move(response)]() mutable {
                      SendAndClose(*session, read_timeout, std::move(response));
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

void Advertise(pp::amp::PeerLinkManager& links, const char* protocolId) {
  auto protocols = links.LocalCapability().protocols;
  for (const auto& id : protocols) {
    if (id == protocolId) {
      return;
    }
  }
  protocols.push_back(protocolId);
  links.SetAdvertisedProtocols(std::move(protocols));
}

} // namespace

void AmpLedgerServer::BindAsync(pp::amp::PeerLinkManager& links, AsyncHandler handler, Options options) {
  const auto shared = std::make_shared<const Options>(std::move(options));
  const auto bound = std::make_shared<const AsyncHandler>(std::move(handler));
  const Lane lanes[] = {
      {kProtocolId, kMaxSmallRequestBytes, false,
       std::make_shared<Receiving>(kMaxReceivingPerPeer, std::numeric_limits<size_t>::max()), nullptr},
      {kBulkProtocolId, kMaxMessageBytes, true, std::make_shared<Receiving>(shared->bulkPerPeer, shared->bulkTotal),
       std::make_shared<Receiving>(shared->bulkPerPeer, std::numeric_limits<size_t>::max())},
  };
  for (const auto& lane : lanes) {
    if (shared->advertise) {
      Advertise(links, lane.protocolId);
    }
    links.SetProtocolHandler(lane.protocolId, [&links, bound, shared, lane](pp::amp::LinkHandle link_handle,
                                                                            const std::string& remote_peer_id,
                                                                            const uint32_t channel_id) {
      links.WithLiveLink(link_handle, [&](pp::amp::PeerLink& link) {
        HandleInboundChannel(link, remote_peer_id, channel_id, *bound, *shared, lane);
      });
    });
  }
}

void AmpLedgerServer::BindAsync(pp::amp::PeerLinkManager& links, AsyncHandler handler, IoPost post_io,
                                const std::chrono::milliseconds read_timeout) {
  Options options;
  options.post_io = std::move(post_io);
  options.read_timeout = read_timeout;
  BindAsync(links, std::move(handler), std::move(options));
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

std::vector<std::string> AmpLedgerServer::protocolIds() { return {kProtocolId, kBulkProtocolId}; }

void AmpLedgerServer::Unbind(pp::amp::PeerLinkManager& links) {
  links.RemoveProtocolHandler(kProtocolId);
  links.RemoveProtocolHandler(kBulkProtocolId);
}

} // namespace network
} // namespace pp
