#include "AmpLedgerServer.h"
#include "AmpLedgerTransport.h"
#include "LedgerRpcProtocol.h"
#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/Types.h"
#include "client/Client.h"
#include "crypto/MlDsa.h"
#include "lib/common/BinaryPack.hpp"

#include <gtest/gtest.h>
#include <sodium.h>

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

  pp::AmpLedgerTransport transport(h->runtime_a->Links(), "b", [&h]() { h->PumpBoth(); });

  const std::string payload = "ledger-rpc-payload";
  auto response = transport.roundTrip(payload, std::chrono::seconds(5));
  ASSERT_TRUE(response.isOk()) << response.error().message;
  EXPECT_EQ(response.value(), payload);
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

  pp::AmpLedgerTransport transport(h->runtime_a->Links(), "b", [&h]() { h->PumpBoth(); });
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

  pp::AmpLedgerTransport transport(h->runtime_a->Links(), "b", [&h]() { h->PumpBoth(); });
  auto response = transport.roundTrip("should-not-arrive", std::chrono::milliseconds(200));
  EXPECT_TRUE(response.isError()) << "expected timeout/failure under total loss";
}

} // namespace
