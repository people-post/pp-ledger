#include "ledger/network/AmpLedgerServer.h"
#include "ledger/client/AmpLedgerTransport.h"
#include "ledger/network/InFlightLimit.h"
#include "ledger/network/LedgerAmpRuntime.h"
#include "ledger/network/LedgerRpcProtocol.h"
#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/Types.h"
#include "ledger/client/Client.h"
#include "crypto/MlDsa.h"
#include "ledger/lib/common/BinaryPack.hpp"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <future>
#include <optional>
#include <thread>
#include <vector>

namespace {
using pp::utl::binaryUnpack;

pp::Roe<std::string> DeriveTestPeerId(const pp::amp::ByteVector& identity_public_key) {
  if (identity_public_key.size() != pp::kMlDsa65PublicKeyBytes) {
    return pp::Error("invalid ML-DSA-65 public key size");
  }
  unsigned char hash[crypto_generichash_BYTES_MIN];
  if (crypto_generichash(hash, sizeof(hash), identity_public_key.data(), identity_public_key.size(), nullptr, 0) != 0) {
    return pp::Error("peer id hash failed");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "test1";
  for (size_t i = 0; i < 16; ++i) {
    out.push_back(kHex[hash[i] >> 4]);
    out.push_back(kHex[hash[i] & 0x0f]);
  }
  return out;
}

struct RpcHarness {
  std::shared_ptr<pp::adp::VirtualClock> clock;
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_a;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_b;
  std::unique_ptr<pp::adp::Endpoint> ep_a;
  std::unique_ptr<pp::adp::Endpoint> ep_b;
  pp::amp::MshIdentity alice;
  pp::amp::MshIdentity bob;
  std::unique_ptr<pp::amp::MeshRuntime> runtime_a;
  std::unique_ptr<pp::amp::MeshRuntime> runtime_b;
  std::string peer_id_a;
  std::string peer_id_b;
  std::string ma_a;
  std::string ma_b;

  static pp::Roe<std::unique_ptr<RpcHarness>> Create() {
    auto h = std::make_unique<RpcHarness>();
    h->clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
    h->hub = pp::adp::MemoryDatagramIo::MakeHub();
    const auto addr_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
    const auto addr_b = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
    h->io_a = std::make_shared<pp::adp::MemoryDatagramIo>(h->hub, addr_a);
    h->io_b = std::make_shared<pp::adp::MemoryDatagramIo>(h->hub, addr_b);
    h->ep_a = std::make_unique<pp::adp::Endpoint>(h->io_a, h->clock);
    h->ep_b = std::make_unique<pp::adp::Endpoint>(h->io_b, h->clock);
    h->ep_b->SetAcceptEnabled(true);

    auto alice_keys = pp::MlDsa::GenerateKeyPair();
    auto bob_keys = pp::MlDsa::GenerateKeyPair();
    if (!alice_keys || !bob_keys) {
      return pp::Error("keygen failed");
    }
    h->alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
    h->alice.ml_dsa_public_key = std::move(alice_keys->public_key);
    h->bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
    h->bob.ml_dsa_public_key = std::move(bob_keys->public_key);

    auto id_a = DeriveTestPeerId(h->alice.ml_dsa_public_key);
    auto id_b = DeriveTestPeerId(h->bob.ml_dsa_public_key);
    if (!id_a || !id_b) {
      return pp::Error("peer id failed");
    }
    h->peer_id_a = *id_a;
    h->peer_id_b = *id_b;

    pp::amp::PeerLinkConfig link_cfg;
    link_cfg.peer_id_from_identity = [](const pp::amp::ByteVector& pk) {
      auto id = DeriveTestPeerId(pk);
      return id ? *id : std::string{};
    };

    h->runtime_a = std::make_unique<pp::amp::MeshRuntime>(*h->ep_a, h->alice, h->peer_id_a, link_cfg);
    h->runtime_b = std::make_unique<pp::amp::MeshRuntime>(*h->ep_b, h->bob, h->peer_id_b, link_cfg);
    h->runtime_a->Start();
    h->runtime_b->Start();

    auto ma_b = pp::amp::FormatAdpMultiaddr(addr_b, h->peer_id_b);
    auto ma_a = pp::amp::FormatAdpMultiaddr(addr_a, h->peer_id_a);
    if (!ma_b || !ma_a) {
      return pp::Error("multiaddr failed");
    }
    h->ma_a = *ma_a;
    h->ma_b = *ma_b;
    return h;
  }

  void PumpBoth() {
    runtime_a->Drive();
    runtime_b->Drive();
  }

  bool Associate() {
    runtime_b->Links().SetLocalListenMultiaddrs({ma_b});
    if (!runtime_a->Links().RegisterEndpoint("b", ma_b)) {
      return false;
    }
    bool done = false;
    bool ok = false;
    runtime_a->Links().EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe r) {
      ok = r.isOk();
      done = true;
    });
    for (size_t i = 0; i < 500 && (!done || !runtime_a->Links().IsConnected("b")); ++i) {
      PumpBoth();
    }
    return ok && runtime_a->Links().IsConnected("b");
  }
};

class AmpLedgerRpcTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AmpLedgerRpcTest, RoundTripEcho) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; });

  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });

  const std::string payload = "ledger-rpc-payload";
  auto response = transport.roundTrip(payload, std::chrono::seconds(5));
  ASSERT_TRUE(response.isOk()) << response.error().message;
  EXPECT_EQ(response.value(), payload);
}

// A peer that opens an RPC channel and never sends must not hold it: the
// server's channel read timeout resets it (the opener sees the channel close).
TEST_F(AmpLedgerRpcTest, ServerDropsChannelThatNeverSends) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; }, {}, {},
                                     std::chrono::milliseconds(2000));

  std::optional<uint32_t> channel;
  h->runtime_a->Links().OpenChannel("b", pp::ledger::rpc::kProtocolId,
                                    pp::ledger::rpc::LedgerRpcChannelPolicy(std::chrono::milliseconds(0)),
                                    [&](const pp::amp::PeerLinkManager::ChannelRoe& ch) {
                                      if (ch) {
                                        channel = ch.value();
                                      }
                                    });
  auto state = [&]() {
    auto* link = h->runtime_a->Links().FindLink("b");
    return link && link->Mux() ? link->Mux()->State(*channel) : pp::amp::ChannelState::Closed;
  };
  for (size_t i = 0; i < 200 && (!channel || state() != pp::amp::ChannelState::Open); ++i) {
    h->PumpBoth();
  }
  ASSERT_TRUE(channel.has_value());
  ASSERT_EQ(state(), pp::amp::ChannelState::Open);

  h->clock->Advance(1000);
  for (size_t i = 0; i < 20; ++i) {
    h->PumpBoth();
  }
  EXPECT_EQ(state(), pp::amp::ChannelState::Open);  // within the read timeout

  h->clock->Advance(1500);
  for (size_t i = 0; i < 20 && state() == pp::amp::ChannelState::Open; ++i) {
    h->PumpBoth();
  }
  EXPECT_EQ(state(), pp::amp::ChannelState::Closed);
}

// B034: a record of a mebibyte (the largest account meta by default) travels, as blocks up to
// maxBlockBytes do; the RPC no longer stops at 512 KiB.
TEST_F(AmpLedgerRpcTest, AMebibyteTravels) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; });
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  const std::string payload(1024 * 1024, 'm');
  auto response = transport.roundTrip(payload, std::chrono::seconds(30));
  ASSERT_TRUE(response.isOk()) << response.error().message;
  EXPECT_EQ(response.value().size(), payload.size());
}

// B034: a peer has at most kMaxReceivingPerPeer requests arriving at once; one more channel is
// reset, so a peer cannot make the node buffer more than that many messages.
TEST_F(AmpLedgerRpcTest, APeerHasAFewRequestsArrivingAtOnce) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; }, {}, {},
                                     std::chrono::milliseconds(60000));
  std::vector<uint32_t> channels;
  for (size_t i = 0; i <= pp::ledger::rpc::kMaxReceivingPerPeer; ++i) {
    h->runtime_a->Links().OpenChannel("b", pp::ledger::rpc::kProtocolId,
                                      pp::ledger::rpc::LedgerRpcChannelPolicy(std::chrono::milliseconds(0)),
                                      [&](const pp::amp::PeerLinkManager::ChannelRoe& ch) {
                                        if (ch) {
                                          channels.push_back(ch.value());
                                        }
                                      });
  }
  auto state = [&](uint32_t id) {
    auto* link = h->runtime_a->Links().FindLink("b");
    return link && link->Mux() ? link->Mux()->State(id) : pp::amp::ChannelState::Closed;
  };
  for (size_t i = 0; i < 400; ++i) {
    h->PumpBoth();
  }
  ASSERT_EQ(channels.size(), pp::ledger::rpc::kMaxReceivingPerPeer + 1);
  size_t open = 0;
  for (uint32_t id : channels) {
    open += state(id) == pp::amp::ChannelState::Open ? 1 : 0;
  }
  EXPECT_EQ(open, pp::ledger::rpc::kMaxReceivingPerPeer) << "one past the limit is reset";
}

// lr8: large requests travel on their own protocol, a few at once per peer (M12: a flood of them
// filled the node's socket and starved every peer).
TEST_F(AmpLedgerRpcTest, APeerHasFewerLargeRequestsArrivingAtOnce) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; }, {}, {},
                                     std::chrono::milliseconds(60000));
  std::vector<uint32_t> channels;
  for (size_t i = 0; i <= pp::ledger::rpc::kMaxBulkReceivingPerPeer; ++i) {
    h->runtime_a->Links().OpenChannel("b", pp::ledger::rpc::kBulkProtocolId,
                                      pp::ledger::rpc::LedgerRpcChannelPolicy(std::chrono::milliseconds(0)),
                                      [&](const pp::amp::PeerLinkManager::ChannelRoe& ch) {
                                        if (ch) {
                                          channels.push_back(ch.value());
                                        }
                                      });
  }
  for (size_t i = 0; i < 400; ++i) {
    h->PumpBoth();
  }
  ASSERT_EQ(channels.size(), pp::ledger::rpc::kMaxBulkReceivingPerPeer + 1);
  size_t open = 0;
  for (uint32_t id : channels) {
    auto* link = h->runtime_a->Links().FindLink("b");
    open += link && link->Mux() && link->Mux()->State(id) == pp::amp::ChannelState::Open ? 1 : 0;
  }
  EXPECT_EQ(open, pp::ledger::rpc::kMaxBulkReceivingPerPeer) << "one past the limit is reset";
}

namespace {

std::string RefusalName(const pp::network::AmpLedgerServer::Refusal why) {
  switch (why) {
  case pp::network::AmpLedgerServer::Refusal::NotAllowed:
    return "not allowed";
  case pp::network::AmpLedgerServer::Refusal::PeerBusy:
    return "peer busy";
  case pp::network::AmpLedgerServer::Refusal::NodeBusy:
    return "node busy";
  }
  return "";
}

/** Echo server with `options`; counts the requests it was handed. */
void BindEcho(pp::amp::PeerLinkManager& links, pp::network::AmpLedgerServer::Options options, std::atomic<int>& handled) {
  options.refusal = RefusalName;
  pp::network::AmpLedgerServer::BindAsync(
      links,
      [&handled](std::string, std::string body, pp::network::AmpLedgerServer::Reply reply) {
        ++handled;
        reply(std::move(body));
      },
      std::move(options));
}

} // namespace

// lr8: a refused large request is answered at once, before its body is sent — the client reads the
// server's reply (in pp-ledger, E_BUSY with a retry-after) instead of timing out.
TEST_F(AmpLedgerRpcTest, ALargeRequestPastTheNodesShareIsAnsweredAtOnce) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  std::atomic<int> handled{0};
  pp::network::AmpLedgerServer::Options options;
  options.bulkTotal = 0;
  BindEcho(h->runtime_b->Links(), std::move(options), handled);
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });

  auto refused = transport.roundTrip(std::string(1024 * 1024, 'm'), std::chrono::seconds(30));
  ASSERT_TRUE(refused.isOk()) << refused.error().message;
  EXPECT_EQ(refused.value(), "node busy");
  auto small = transport.roundTrip(std::string(1000, 's'), std::chrono::seconds(30));
  ASSERT_TRUE(small.isOk()) << small.error().message;
  EXPECT_EQ(small.value().size(), 1000u) << "small requests are not held up";
  EXPECT_EQ(handled.load(), 1);
}

// lr8 (M14): a large request holds its place until it is answered, so large requests waiting in the
// server's queue count too — they cannot fill the queue that small requests share.
TEST_F(AmpLedgerRpcTest, ALargeRequestHoldsItsPlaceUntilAnswered) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  std::vector<pp::network::AmpLedgerServer::Reply> pending;
  pp::network::AmpLedgerServer::Options options;
  options.bulkTotal = 1;
  options.refusal = RefusalName;
  pp::network::AmpLedgerServer::BindAsync(
      h->runtime_b->Links(),
      [&pending](std::string, std::string, pp::network::AmpLedgerServer::Reply reply) {
        pending.push_back(std::move(reply)); // answered later
      },
      std::move(options));
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  // A block: chain work, so the client sends two at once (forwarded requests keep one place free).
  pp::Client::Request block;
  block.version = pp::Client::Request::VERSION;
  block.type = pp::Client::T_REQ_BLOCK_ADD;
  block.payload = std::string(pp::ledger::rpc::kMaxSmallRequestBytes + 1, 'm');
  const std::string large = pp::utl::binaryPack(block);

  std::optional<std::string> first;
  transport.roundTripAsync(large, std::chrono::seconds(30), [&first](pp::ILedgerTransport::Roe<std::string> r) {
    first = r ? r.value() : "error: " + r.error().message;
  });
  for (size_t i = 0; i < 2000 && pending.empty(); ++i) {
    h->PumpBoth();
  }
  ASSERT_EQ(pending.size(), 1u) << "the first is received and waits for its answer";

  auto second = transport.roundTrip(large, std::chrono::seconds(30));
  ASSERT_TRUE(second.isOk()) << second.error().message;
  EXPECT_EQ(second.value(), "node busy") << "its body has arrived, but it still holds the node's place";

  pending.front()("done");
  for (size_t i = 0; i < 400 && !first; ++i) {
    h->PumpBoth();
  }
  EXPECT_EQ(first, "done");
  std::optional<std::string> third;
  transport.roundTripAsync(large, std::chrono::seconds(30), [&third](pp::ILedgerTransport::Roe<std::string> r) {
    third = r ? r.value() : "error: " + r.error().message;
  });
  for (size_t i = 0; i < 2000 && pending.size() < 2; ++i) {
    h->PumpBoth();
  }
  EXPECT_EQ(pending.size(), 2u) << "answered, the place is free again";
  EXPECT_FALSE(third) << "not refused";
}

// lr8 (LR009): past the requests in flight and those allowed to wait, one more is answered busy at
// once (E_BUSY) — a relay passes that on — instead of waiting out its deadline; chain work still waits.
TEST_F(AmpLedgerRpcTest, PastTheWaitingLimitARequestIsBusyAtOnce) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  std::vector<pp::network::AmpLedgerServer::Reply> pending; // never answered
  pp::network::AmpLedgerServer::BindAsync(
      h->runtime_b->Links(),
      [&pending](std::string, std::string, pp::network::AmpLedgerServer::Reply reply) {
        pending.push_back(std::move(reply));
      },
      pp::network::AmpLedgerServer::Options{});
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  auto request = [](uint32_t type) {
    pp::Client::Request r;
    r.version = pp::Client::Request::VERSION;
    r.type = type;
    return pp::utl::binaryPack(r);
  };
  const size_t normal = pp::ledger::rpc::kMaxReceivingPerPeer - pp::ledger::rpc::kReservedForDuties +
                        pp::ledger::rpc::kMaxWaitingPerPeer;
  size_t answered = 0;
  for (size_t i = 0; i < normal; ++i) {
    transport.roundTripAsync(request(pp::Client::T_REQ_ACCOUNT_GET), std::chrono::seconds(30),
                             [&answered](pp::ILedgerTransport::Roe<std::string>) { ++answered; });
  }
  std::optional<pp::ILedgerTransport::Roe<std::string>> busy;
  transport.roundTripAsync(request(pp::Client::T_REQ_ACCOUNT_GET), std::chrono::seconds(30),
                           [&busy](pp::ILedgerTransport::Roe<std::string> r) { busy = std::move(r); });
  std::optional<pp::ILedgerTransport::Roe<std::string>> duty;
  transport.roundTripAsync(request(pp::Client::T_REQ_BLOCK_GET), std::chrono::seconds(30),
                           [&duty](pp::ILedgerTransport::Roe<std::string> r) { duty = std::move(r); });
  const size_t started = pp::ledger::rpc::kMaxReceivingPerPeer - pp::ledger::rpc::kReservedForDuties + 1;
  for (size_t i = 0; i < 400 && (!busy || pending.size() < started); ++i) {
    h->PumpBoth();
  }
  ASSERT_TRUE(busy);
  ASSERT_FALSE(busy->isOk());
  EXPECT_EQ(busy->error().code, pp::Client::E_BUSY);
  EXPECT_EQ(answered, 0u) << "the others wait their turn";
  EXPECT_FALSE(duty) << "chain work is not refused: it has its own places";
  EXPECT_EQ(pending.size(), started) << "the normal requests in flight, and the duty in a reserved place";
}

// lr8: a peer the node does not serve is refused when its large request's channel opens, not after
// the body has arrived; a small request (HEALTH) still reaches the server, which checks it there.
TEST_F(AmpLedgerRpcTest, AStrangersLargeRequestIsRefusedWhenItsChannelOpens) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  std::atomic<int> handled{0};
  std::string asked;
  pp::network::AmpLedgerServer::Options options;
  options.admit = [&asked](const std::string& peer) {
    asked = peer;
    return false;
  };
  BindEcho(h->runtime_b->Links(), std::move(options), handled);
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });

  auto refused = transport.roundTrip(std::string(pp::ledger::rpc::kMaxSmallRequestBytes + 1, 'm'),
                                     std::chrono::seconds(30));
  ASSERT_TRUE(refused.isOk()) << refused.error().message;
  EXPECT_EQ(refused.value(), "not allowed");
  EXPECT_EQ(asked, h->peer_id_a);
  auto small = transport.roundTrip(std::string(pp::ledger::rpc::kMaxSmallRequestBytes, 's'), std::chrono::seconds(30));
  ASSERT_TRUE(small.isOk()) << small.error().message;
  EXPECT_EQ(handled.load(), 1);
}

// lr8: the small protocol takes no request over kMaxSmallRequestBytes: its channel is reset at the
// first fragment, so a large body cannot bypass the large requests' limits.
TEST_F(AmpLedgerRpcTest, ALargeBodyOnTheSmallProtocolIsReset) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());
  std::atomic<int> handled{0};
  BindEcho(h->runtime_b->Links(), {}, handled);
  std::optional<uint32_t> channel;
  h->runtime_a->Links().OpenChannel("b", pp::ledger::rpc::kProtocolId,
                                    pp::ledger::rpc::LedgerRpcChannelPolicy(std::chrono::milliseconds(0)),
                                    [&](const pp::amp::PeerLinkManager::ChannelRoe& ch) {
                                      if (ch) {
                                        channel = ch.value();
                                      }
                                    });
  for (size_t i = 0; i < 100; ++i) {
    h->PumpBoth();
  }
  ASSERT_TRUE(channel);
  std::string closed;
  auto session = h->runtime_a->Links().BindChannel(
      "b", *channel, pp::ledger::rpc::LedgerRpcChannelPolicy(std::chrono::milliseconds(0)),
      [](pp::Roe<std::vector<uint8_t>>) { return false; }, [&](const char* reason) { closed = reason ? reason : ""; });
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->EnqueueOutbound(std::vector<uint8_t>(pp::ledger::rpc::kMaxSmallRequestBytes + 1, 'x')));
  for (size_t i = 0; i < 400 && closed.empty(); ++i) {
    h->PumpBoth();
  }
  EXPECT_EQ(closed, "peer_reset");
  EXPECT_EQ(handled.load(), 0);
}

TEST_F(AmpLedgerRpcTest, ClientRequestRoundTrip) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) {
    auto req = pp::utl::binaryUnpack<pp::Client::Request>(body);
    if (!req) {
      pp::Client::Response err;
      err.version = pp::Client::Response::VERSION;
      err.errorCode = 1;
      err.payload = req.error().message;
      return pp::utl::binaryPack(err);
    }
    pp::Client::Response resp;
    resp.version = pp::Client::Response::VERSION;
    resp.errorCode = 0;
    resp.payload = req->payload;
    return pp::utl::binaryPack(resp);
  });

  pp::Client::Request request;
  request.version = pp::Client::Request::VERSION;
  request.type = 42;
  request.payload = "hello-ledger";

  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  auto framed = transport.roundTrip(pp::utl::binaryPack(request), std::chrono::seconds(5));
  ASSERT_TRUE(framed.isOk()) << framed.error().message;

  auto unpacked = pp::utl::binaryUnpack<pp::Client::Response>(framed.value());
  ASSERT_TRUE(unpacked.isOk());
  EXPECT_EQ(unpacked->errorCode, 0);
  EXPECT_EQ(unpacked->payload, request.payload);
}


TEST_F(AmpLedgerRpcTest, RoundTripFailsWhenDatagramsDropped) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; });

  // Drop every outbound datagram from the client after association so the RPC
  // cannot complete — in-process stand-in for L-NET-LOSS (purpose catalog).
  h->io_a->SetDropRate(1.0);

  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  auto response = transport.roundTrip("should-not-arrive", std::chrono::milliseconds(200));
  EXPECT_TRUE(response.isError()) << "expected timeout/failure under total loss";
}


TEST_F(AmpLedgerRpcTest, SuccessThenLossFailsSecondRoundTrip) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; });
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });

  auto first = transport.roundTrip("ok-before-loss", std::chrono::seconds(5));
  ASSERT_TRUE(first.isOk()) << first.error().message;
  EXPECT_EQ(first.value(), "ok-before-loss");

  h->io_a->SetDropRate(1.0);
  auto second = transport.roundTrip("should-fail", std::chrono::milliseconds(200));
  EXPECT_TRUE(second.isError()) << "expected failure after enabling loss";
}

TEST_F(AmpLedgerRpcTest, RoundTripSurvivesReorderWindow) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  // L-NET-REORDER: hold-and-random-release on both directions after association.
  h->io_a->SetReorderWindow(4);
  h->io_b->SetReorderWindow(4);
  h->io_a->SetRngSeed(0);
  h->io_b->SetRngSeed(1);

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; });
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() {
    h->PumpBoth();
    // Drive may emit several datagrams; flush after so held packets still make progress.
    h->io_a->FlushReorder();
    h->io_b->FlushReorder();
  });

  const std::string payload = "reorder-window-payload";
  auto response = transport.roundTrip(payload, std::chrono::seconds(5));
  ASSERT_TRUE(response.isOk()) << response.error().message;
  EXPECT_EQ(response.value(), payload);
}

TEST_F(AmpLedgerRpcTest, RoundTripSurvivesDatagramDuplication) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  // Transport-level duplicate injection (ingress replay of datagrams).
  h->io_a->SetDupRate(1.0);
  h->io_b->SetDupRate(1.0);
  h->io_a->SetRngSeed(3);
  h->io_b->SetRngSeed(4);

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) { return body; });
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });

  auto response = transport.roundTrip("dup-datagram-payload", std::chrono::seconds(5));
  ASSERT_TRUE(response.isOk()) << response.error().message;
  EXPECT_EQ(response.value(), "dup-datagram-payload");
}

TEST_F(AmpLedgerRpcTest, ClientRejectsOversizeRequest) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  bool handler_called = false;
  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [&](const std::string& body) {
    handler_called = true;
    return body;
  });

  // L-ADV-INGRESS: AmpLedgerTransport refuses bodies above kMaxMessageBytes.
  const std::string huge(pp::ledger::rpc::kMaxMessageBytes + 1, 'x');
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  auto response = transport.roundTrip(huge, std::chrono::seconds(2));
  EXPECT_TRUE(response.isError());
  EXPECT_FALSE(handler_called);
}

TEST_F(AmpLedgerRpcTest, TruncatedClientRequestReturnsErrorResponse) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  // L-ADV-INGRESS: malformed / truncated Client::Request is rejected cleanly.
  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [](const std::string& body) {
    auto req = pp::utl::binaryUnpack<pp::Client::Request>(body);
    if (!req) {
      pp::Client::Response err;
      err.version = pp::Client::Response::VERSION;
      err.errorCode = 1;
      err.payload = req.error().message;
      return pp::utl::binaryPack(err);
    }
    pp::Client::Response resp;
    resp.version = pp::Client::Response::VERSION;
    resp.errorCode = 0;
    resp.payload = req->payload;
    return pp::utl::binaryPack(resp);
  });

  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  auto framed = transport.roundTrip(std::string("\x01\x02\x03", 3), std::chrono::seconds(5));
  ASSERT_TRUE(framed.isOk()) << framed.error().message;

  auto unpacked = pp::utl::binaryUnpack<pp::Client::Response>(framed.value());
  ASSERT_TRUE(unpacked.isOk());
  EXPECT_NE(unpacked->errorCode, 0);
  EXPECT_FALSE(unpacked->payload.empty());
}

TEST_F(AmpLedgerRpcTest, IdenticalRequestReplayIsIdempotentEcho) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  size_t handler_calls = 0;
  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), [&](const std::string& body) {
    ++handler_calls;
    return body;
  });

  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });
  const std::string payload = "replay-same-body";
  auto first = transport.roundTrip(payload, std::chrono::seconds(5));
  auto second = transport.roundTrip(payload, std::chrono::seconds(5));
  ASSERT_TRUE(first.isOk()) << first.error().message;
  ASSERT_TRUE(second.isOk()) << second.error().message;
  EXPECT_EQ(first.value(), payload);
  EXPECT_EQ(second.value(), payload);
  EXPECT_EQ(handler_calls, 2u);
}

// Mirrors Server::handleRequest unpack + BeaconServer unsupported-type path.
std::string DispatchClientRequestOrReject(const std::string& body) {
  auto req = pp::utl::binaryUnpack<pp::Client::Request>(body);
  if (!req) {
    pp::Client::Response err;
    err.version = pp::Client::Response::VERSION;
    err.errorCode = 1;
    err.payload = req.error().message;
    return pp::utl::binaryPack(err);
  }
  // Only STATUS is treated as supported in this ingress fixture.
  if (req->type != pp::Client::T_REQ_STATUS) {
    pp::Client::Response err;
    err.version = pp::Client::Response::VERSION;
    err.errorCode = 1;
    err.payload = "Unsupported request type: " + std::to_string(req->type);
    return pp::utl::binaryPack(err);
  }
  pp::Client::Response resp;
  resp.version = pp::Client::Response::VERSION;
  resp.errorCode = 0;
  resp.payload = req->payload;
  return pp::utl::binaryPack(resp);
}

TEST_F(AmpLedgerRpcTest, EmptyRequestBodyReturnsErrorResponse) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), DispatchClientRequestOrReject);
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });

  auto framed = transport.roundTrip(std::string{}, std::chrono::seconds(5));
  ASSERT_TRUE(framed.isOk()) << framed.error().message;
  auto unpacked = pp::utl::binaryUnpack<pp::Client::Response>(framed.value());
  ASSERT_TRUE(unpacked.isOk());
  EXPECT_TRUE(unpacked->isError());
  EXPECT_FALSE(unpacked->payload.empty());
}

TEST_F(AmpLedgerRpcTest, UnknownRequestTypeReturnsErrorResponse) {
  auto created = RpcHarness::Create();
  ASSERT_TRUE(created.isOk());
  auto h = std::move(created.value());
  ASSERT_TRUE(h->Associate());

  pp::network::AmpLedgerServer::Bind(h->runtime_b->Links(), DispatchClientRequestOrReject);
  pp::AmpLedgerTransport transport(*h->runtime_a, "b", [&h]() { h->PumpBoth(); });

  pp::Client::Request request;
  request.version = pp::Client::Request::VERSION;
  request.type = 0xDEADBEEFu;
  request.payload = "nope";

  auto framed = transport.roundTrip(pp::utl::binaryPack(request), std::chrono::seconds(5));
  ASSERT_TRUE(framed.isOk()) << framed.error().message;
  auto unpacked = pp::utl::binaryUnpack<pp::Client::Response>(framed.value());
  ASSERT_TRUE(unpacked.isOk());
  EXPECT_TRUE(unpacked->isError());
  EXPECT_NE(unpacked->payload.find("Unsupported request type"), std::string::npos);
}

} // namespace

// --- Threaded mode: LedgerAmpRuntime pump threads own the stacks ---

namespace {

pp::amp::PeerLinkConfig TestLinkConfig() {
  pp::amp::PeerLinkConfig link_cfg;
  link_cfg.peer_id_from_identity = [](const pp::amp::ByteVector& pk) {
    auto id = DeriveTestPeerId(pk);
    return id ? *id : std::string{};
  };
  return link_cfg;
}

pp::Roe<pp::network::LedgerAmpConfig> TestAmpConfig() {
  auto keys = pp::MlDsa::GenerateKeyPair();
  if (!keys) {
    return pp::Error("keygen failed");
  }
  pp::network::LedgerAmpConfig cfg;
  cfg.identity.ml_dsa_secret_key = std::move(keys->secret_key);
  cfg.identity.ml_dsa_public_key = std::move(keys->public_key);
  auto id = DeriveTestPeerId(cfg.identity.ml_dsa_public_key);
  if (!id) {
    return id.error();
  }
  cfg.local_peer_id = *id;
  cfg.link_config = TestLinkConfig();
  return cfg;
}

/** Two LedgerAmpRuntimes on an in-memory hub with wall clocks; B serves an echo. */
struct ThreadedPair {
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub = pp::adp::MemoryDatagramIo::MakeHub();
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_a;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_b;
  pp::network::LedgerAmpRuntime a;
  pp::network::LedgerAmpRuntime b;

  pp::Roe<void> Start() {
    io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, pp::adp::IpEndpoint::V4(10, 1, 0, 1, 1000));
    io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, pp::adp::IpEndpoint::V4(10, 1, 0, 2, 2000));
    auto cfg_a = TestAmpConfig();
    auto cfg_b = TestAmpConfig();
    if (!cfg_a || !cfg_b) {
      return pp::Error("config failed");
    }
    if (auto r = b.StartForTest(io_b, std::make_shared<pp::adp::WallClock>(), std::move(*cfg_b)); !r) {
      return r;
    }
    pp::network::AmpLedgerServer::Bind(
        b.links(), [](const std::string& body) { return body; }, {},
        [this](std::function<void()> task) { b.post(std::move(task)); });
    return a.StartForTest(io_a, std::make_shared<pp::adp::WallClock>(), std::move(*cfg_a));
  }

  /** MemoryDatagramIo knobs are io-thread state: change them on A's pump thread. */
  void SetClientDropRate(double rate) {
    std::promise<void> done;
    a.post([&]() {
      io_a->SetDropRate(rate);
      done.set_value();
    });
    done.get_future().wait();
  }
};

} // namespace

TEST_F(AmpLedgerRpcTest, ThreadedRoundTripsFromManyCallerThreads) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));

  constexpr int kThreads = 4;
  constexpr int kCallsPerThread = 10;
  std::atomic<int> failures{0};
  std::vector<std::thread> callers;
  for (int t = 0; t < kThreads; ++t) {
    callers.emplace_back([&, t]() {
      for (int i = 0; i < kCallsPerThread; ++i) {
        const std::string payload = "t" + std::to_string(t) + "-" + std::to_string(i);
        auto response = transport.roundTrip(payload, std::chrono::seconds(5));
        if (!response || response.value() != payload) {
          failures++;
        }
      }
    });
  }
  for (auto& c : callers) {
    c.join();
  }
  EXPECT_EQ(failures.load(), 0);
}

// Callbacks that land after a timeout must not touch the caller's (gone) stack
// frame, and the transport must stay usable once the link recovers.
TEST_F(AmpLedgerRpcTest, ThreadedTimeoutThenRecovery) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));
  ASSERT_TRUE(transport.roundTrip("warm-up", std::chrono::seconds(5)).isOk());

  pair.SetClientDropRate(1.0);
  auto lost = transport.roundTrip("lost", std::chrono::milliseconds(200));
  EXPECT_TRUE(lost.isError());

  pair.SetClientDropRate(0.0);
  auto response = transport.roundTrip("after-loss", std::chrono::seconds(5));
  ASSERT_TRUE(response.isOk()) << response.error().message;
  EXPECT_EQ(response.value(), "after-loss");
}

// The pump thread is the only driver: blocking on io work from it would deadlock.
TEST_F(AmpLedgerRpcTest, ThreadedRoundTripOnPumpThreadIsRefused) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));

  std::promise<std::string> error;
  pair.a.post([&]() {
    auto r = transport.roundTrip("from-pump", std::chrono::seconds(1));
    error.set_value(r ? std::string{} : r.error().message);
  });
  auto f = error.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_NE(f.get().find("pump thread"), std::string::npos);
}

TEST_F(AmpLedgerRpcTest, ThreadedAsyncRoundTripCompletesOnIoLane) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));

  std::promise<std::pair<std::string, bool>> done;
  transport.roundTripAsync("async-payload", std::chrono::seconds(5),
                           [&](pp::ILedgerTransport::Roe<std::string> r) {
                             done.set_value({r ? r.value() : r.error().message, pair.a.onIoThread()});
                           });
  auto f = done.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  auto [body, onIoLane] = f.get();
  EXPECT_EQ(body, "async-payload");
  EXPECT_TRUE(onIoLane);
}

// load-resilience lr6: at most `limit` started per peer; the rest start in order as places free up,
// and one that gave up while waiting is skipped.
TEST(InFlightLimitTest, StartsInOrderAsPlacesFreeUp) {
  pp::network::InFlightLimit limit(2);
  std::vector<int> started;
  std::vector<bool> live(5, true);
  for (int i = 0; i < 5; ++i) {
    limit.run("p", {[&live, i]() { return static_cast<bool>(live[i]); }, [&started, i]() { started.push_back(i); }});
  }
  EXPECT_EQ(started, (std::vector<int>{0, 1}));
  EXPECT_EQ(limit.waiting("p"), 3u);
  limit.run("q", {[]() { return true; }, [&started]() { started.push_back(99); }});
  EXPECT_EQ(started.back(), 99) << "another peer has its own places";
  live[2] = false; // gave up waiting
  limit.release("p");
  EXPECT_EQ(started.back(), 3);
  limit.release("p");
  limit.release("p");
  EXPECT_EQ(started.back(), 4);
  EXPECT_EQ(limit.inFlight("p"), 1u);
  limit.release("p");
  EXPECT_EQ(limit.inFlight("p"), 0u);
  EXPECT_EQ(limit.waiting("p"), 0u);
}

// lr8 (LR009): a node's own chain work keeps places its forwards cannot take, and goes first.
TEST(InFlightLimitTest, DutiesHaveReservedPlacesAndGoFirst) {
  using Class = pp::network::InFlightLimit::Class;
  pp::network::InFlightLimit limit(4, /*reserved=*/1);
  std::vector<std::string> started;
  auto waiter = [&started](std::string name) {
    return pp::network::InFlightLimit::Waiter{[]() { return true; }, [&started, name]() { started.push_back(name); }};
  };
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limit.run("p", waiter("n" + std::to_string(i)), Class::Normal));
  }
  EXPECT_EQ(started, (std::vector<std::string>{"n0", "n1", "n2"})) << "normal requests leave one place";
  EXPECT_TRUE(limit.run("p", waiter("d0"), Class::Duty));
  EXPECT_EQ(started.back(), "d0") << "a duty takes the reserved place at once";
  EXPECT_TRUE(limit.run("p", waiter("d1"), Class::Duty));
  EXPECT_EQ(started.back(), "d0") << "full: the duty waits";
  limit.release("p");
  EXPECT_EQ(started.back(), "d1") << "a waiting duty goes before waiting normal requests";
  limit.release("p");
  EXPECT_EQ(started.back(), "d1") << "normal requests may not take the place duties keep";
  limit.release("p");
  EXPECT_EQ(started.back(), "n3");
}

// lr8 (LR009): past `maxWaiting`, a normal request is refused at once — its caller answers busy now
// instead of letting it wait out its deadline; a duty always waits.
TEST(InFlightLimitTest, NormalRequestsWaitBoundedDutiesAlways) {
  using Class = pp::network::InFlightLimit::Class;
  pp::network::InFlightLimit limit(1, /*reserved=*/0, /*maxWaiting=*/2);
  auto waiter = []() { return pp::network::InFlightLimit::Waiter{[]() { return true; }, []() {}}; };
  EXPECT_TRUE(limit.run("p", waiter()));
  EXPECT_TRUE(limit.run("p", waiter()));
  EXPECT_TRUE(limit.run("p", waiter()));
  EXPECT_FALSE(limit.run("p", waiter())) << "two wait already";
  EXPECT_TRUE(limit.run("p", waiter(), Class::Duty));
  EXPECT_EQ(limit.waiting("p"), 3u);
  EXPECT_TRUE(limit.run("q", waiter())) << "another peer has its own queue";
}

// load-resilience lr6: a client sending more at once than the server takes from one peer waits its
// turn instead of meeting the server's refusal of one more channel.
TEST_F(AmpLedgerRpcTest, ThreadedBurstPastThePerPeerLimitWaitsItsTurn) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));
  ASSERT_TRUE(transport.roundTrip("warm-up", std::chrono::seconds(5)).isOk());

  constexpr size_t kCalls = 3 * pp::ledger::rpc::kMaxReceivingPerPeer;
  std::mutex mu;
  std::condition_variable cv;
  size_t answered = 0;
  std::vector<std::string> errors;
  for (size_t i = 0; i < kCalls; ++i) {
    transport.roundTripAsync("burst-" + std::to_string(i), std::chrono::seconds(10),
                             [&](pp::ILedgerTransport::Roe<std::string> r) {
                               std::lock_guard<std::mutex> lock(mu);
                               if (!r) {
                                 errors.push_back(r.error().message);
                               }
                               ++answered;
                               cv.notify_all();
                             });
  }
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(20), [&]() { return answered == kCalls; }));
  EXPECT_TRUE(errors.empty()) << errors.size() << " failed, e.g. " << errors.front();
}

// Unlike the blocking call, the async one may start from the pump thread.
TEST_F(AmpLedgerRpcTest, ThreadedAsyncRoundTripFromPumpThread) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));

  std::promise<std::string> done;
  pair.a.post([&]() {
    transport.roundTripAsync("from-pump", std::chrono::seconds(5),
                             [&](pp::ILedgerTransport::Roe<std::string> r) {
                               done.set_value(r ? r.value() : r.error().message);
                             });
  });
  auto f = done.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_EQ(f.get(), "from-pump");
}

TEST_F(AmpLedgerRpcTest, ThreadedAsyncRoundTripTimesOut) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));
  ASSERT_TRUE(transport.roundTrip("warm-up", std::chrono::seconds(5)).isOk());
  pair.SetClientDropRate(1.0);

  std::promise<bool> done;
  transport.roundTripAsync("lost", std::chrono::milliseconds(200),
                           [&](pp::ILedgerTransport::Roe<std::string> r) { done.set_value(r.isError()); });
  auto f = done.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_TRUE(f.get());
  pair.SetClientDropRate(0.0);
}

// post() from the pump thread (inside Drive, holding amp's io lock) racing
// posts from another thread must not deadlock (lock-order regression).
TEST_F(AmpLedgerRpcTest, ThreadedPostFromPumpAndCallerConcurrently) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  constexpr int kPosts = 2000;
  std::atomic<int> ran{0};
  std::promise<void> pumpDone;
  pair.a.post([&]() {
    for (int i = 0; i < kPosts; ++i) {
      pair.a.post([&]() { ++ran; });
    }
    pumpDone.set_value();
  });
  auto caller = std::async(std::launch::async, [&]() {
    for (int i = 0; i < kPosts; ++i) {
      pair.a.post([&]() { ++ran; });
    }
  });
  ASSERT_EQ(caller.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  ASSERT_EQ(pumpDone.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (ran.load() < 2 * kPosts && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(ran.load(), 2 * kPosts);
}

// Regressions for pp-cpp-amp < v2.13.1: reliable messages that did not fit
// the ADP window (128 frames ≈ 115 KB) were refused and the channel closed,
// so the reply never arrived (BlockSync's parallel block fetches timed out).
namespace {

void ExpectEchoes(pp::AmpLedgerTransport &transport, const std::vector<size_t> &sizes) {
  std::vector<std::promise<std::string>> results(sizes.size());
  for (size_t i = 0; i < sizes.size(); ++i) {
    const std::string payload(sizes[i], static_cast<char>('a' + i));
    transport.roundTripAsync(payload, std::chrono::seconds(10),
                             [&results, i, size = payload.size()](pp::ILedgerTransport::Roe<std::string> r) {
                               results[i].set_value(r ? (r.value().size() == size ? "" : "size mismatch")
                                                      : r.error().message);
                             });
  }
  for (size_t i = 0; i < results.size(); ++i) {
    auto f = results[i].get_future();
    ASSERT_EQ(f.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    EXPECT_EQ(f.get(), "") << "message " << i << " (" << sizes[i] << " bytes)";
  }
}

} // namespace

TEST_F(AmpLedgerRpcTest, ThreadedConcurrentLargeRoundTrips) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));
  for (int round = 0; round < 3; ++round) {
    ExpectEchoes(transport, {48 * 1024, 48 * 1024 + 1, 48 * 1024 + 2, 48 * 1024 + 3});
  }
}

TEST_F(AmpLedgerRpcTest, ThreadedMessageLargerThanTransportWindow) {
  ThreadedPair pair;
  ASSERT_TRUE(pair.Start().isOk());
  pp::AmpLedgerTransport transport(pair.a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", pair.b.listenMultiaddr()));
  ExpectEchoes(transport, {200 * 1024});
  ExpectEchoes(transport, {pp::ledger::rpc::kMaxMessageBytes - 64});
}

// Regression for pp-cpp-amp < v2.13.2: over real UDP a burst loses a packet;
// the receiver ACKed packets it then dropped (beyond its 64-entry replay
// window), so they were never resent and the link's reliable stream stalled.
TEST_F(AmpLedgerRpcTest, OsUdpConcurrentLargeRoundTrips) {
  auto bind = []() { return pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0)); };
  auto io_a = bind();
  auto io_b = bind();
  ASSERT_TRUE(io_a.isOk() && io_b.isOk());
  auto cfg_a = TestAmpConfig();
  auto cfg_b = TestAmpConfig();
  ASSERT_TRUE(cfg_a.isOk() && cfg_b.isOk());
  pp::network::LedgerAmpRuntime a;
  pp::network::LedgerAmpRuntime b;
  ASSERT_TRUE(b.StartForTest(std::shared_ptr<pp::adp::DatagramIo>(std::move(*io_b)),
                             std::make_shared<pp::adp::WallClock>(), std::move(cfg_b.value()))
                  .isOk());
  pp::network::AmpLedgerServer::Bind(
      b.links(), [](const std::string& body) { return body; }, {},
      [&b](std::function<void()> task) { b.post(std::move(task)); });
  ASSERT_TRUE(a.StartForTest(std::shared_ptr<pp::adp::DatagramIo>(std::move(*io_a)),
                             std::make_shared<pp::adp::WallClock>(), std::move(cfg_a.value()))
                  .isOk());
  pp::AmpLedgerTransport transport(a, "b");
  ASSERT_TRUE(transport.registerEndpoint("b", b.listenMultiaddr()));
  for (int round = 0; round < 10; ++round) {
    ExpectEchoes(transport, {90 * 1024, 90 * 1024 + 1, 90 * 1024 + 2, 90 * 1024 + 3});
  }
}

