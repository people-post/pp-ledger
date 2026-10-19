#include "server/Server.h"
#include "client/AmpLedgerTransport.h"
#include "ledger/network/AmpLedgerServer.h"
#include "ledger/network/HostedAmpRuntime.h"
#include "ledger/network/LedgerAmpRuntime.h"
#include "ledger/network/amp/AmpIdentity.h"
#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"
#include "testing/TestAmpHost.h"
#include "testing/TestTempPath.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

using namespace pp;
using namespace std::chrono_literals;

// platform-integration pi1: a ledger node runs on a host's Amp runtime — pp-node's mesh — with the
// host's socket and identity; the host keeps its runtime-wide settings, and once the node stops
// nothing calls back into it while the host runs on.

namespace {

constexpr const char *kHostProtocol = pp::test::TestAmpHost::kProtocol;
using Host = pp::test::TestAmpHost;

network::LedgerAmpConfig freshIdentity() { return pp::test::freshAmpIdentity(); }

/** A ledger node answering request type 1 with "echo:<payload>". */
class LedgerNode : public Server {
public:
  std::atomic<int> identityAsked{0};
  std::atomic<int> handled{0};
  std::promise<void> started;
  using Server::ampRuntime;
  using Server::listenMultiaddr;

protected:
  std::string getSignatureFileName() const override { return ".test"; }
  std::string getLogFileName() const override { return "test.log"; }
  std::string getServerName() const override { return "LedgerNode"; }
  Service::Roe<void> onStart() override {
    handle(1, [this](const Client::Request &request) -> Roe<std::string> {
      ++handled;
      return "echo:" + request.payload;
    });
    return startAmpServer(18519, [this]() -> Service::Roe<std::string> {
      ++identityAsked;
      return Service::Error(-1, "a node on a host has the host's identity");
    });
  }
  void runLoop() override {
    started.set_value();
    while (!isStopSet()) {
      serveRequestsFor(10ms);
    }
  }
};

std::string request(const std::string &payload) {
  Client::Request req;
  req.type = 1;
  req.payload = payload;
  return utl::binaryPack(req);
}

std::string payloadOf(const AmpLedgerTransport::Roe<std::string> &response) {
  if (!response) {
    return "error: " + response.error().message;
  }
  auto unpacked = utl::binaryUnpack<Client::Response>(response.value());
  return unpacked ? unpacked.value().payload : "unreadable";
}

class HostedAmpTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = pp::test::uniqueTempPath("pp-ledger-hosted-amp-test");
    std::filesystem::create_directories(dir_);
    host_.start(hub_, pp::adp::IpEndpoint::V4(10, 2, 0, 1, 1000));
    auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub_, pp::adp::IpEndpoint::V4(10, 2, 0, 2, 2000));
    ASSERT_TRUE(peer_.StartForTest(io, std::make_shared<pp::adp::WallClock>(), freshIdentity()).isOk());
  }
  void TearDown() override {
    peer_.Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** Run `node` on the host until it serves; the caller stops it. */
  std::thread runNode(LedgerNode &node) {
    node.runOnHost(host_.forLedger());
    std::thread thread([this, &node]() { EXPECT_TRUE(node.run(dir_.string()).isOk()); });
    EXPECT_EQ(node.started.get_future().wait_for(10s), std::future_status::ready);
    return thread;
  }

  std::shared_ptr<pp::adp::MemoryDatagramHub> hub_ = pp::adp::MemoryDatagramIo::MakeHub();
  Host host_;
  /** A standalone ledger peer on the same network. */
  network::LedgerAmpRuntime peer_;
  std::filesystem::path dir_;
};

} // namespace

TEST_F(HostedAmpTest, ANodeServesLedgerRpcOnTheHostsRuntime) {
  LedgerNode node;
  std::thread thread = runNode(node);
  EXPECT_EQ(node.listenMultiaddr(), host_.listen) << "the host's address and identity";
  EXPECT_EQ(node.identityAsked.load(), 0) << "no identity of its own";
  EXPECT_FALSE(std::filesystem::exists(dir_ / "keys" / "amp-identity.txt"));
  EXPECT_EQ(host_.stack->Links().LocalCapability().protocols, std::vector<std::string>{kHostProtocol})
      << "the host keeps its advertisement (it adds AmpLedgerServer::protocolIds() itself)";

  AmpLedgerTransport transport(peer_, "node");
  ASSERT_TRUE(transport.registerEndpoint("node", host_.listen));
  EXPECT_EQ(payloadOf(transport.roundTrip(request("hello"), 5s)), "echo:hello");

  node.setStop(true);
  thread.join();
  const int handled = node.handled.load();
  EXPECT_NE(payloadOf(transport.roundTrip(request("again"), 2s)), "echo:again") << "stopped: not served";
  EXPECT_EQ(node.handled.load(), handled);
  EXPECT_TRUE(host_.stack->Runtime().IsStarted()) << "the host runs on";
}

TEST_F(HostedAmpTest, ANodesClientsCallOutOnTheHostsRuntime) {
  pp::network::AmpLedgerServer::Bind(
      peer_.links(), [](const std::string &body) { return body; }, {},
      [this](std::function<void()> task) { peer_.post(std::move(task)); });
  LedgerNode node;
  std::thread thread = runNode(node);
  ASSERT_NE(node.ampRuntime(), nullptr);
  EXPECT_FALSE(node.ampRuntime()->ownsRuntime());

  AmpLedgerTransport out(*node.ampRuntime(), "peer");
  ASSERT_TRUE(out.registerEndpoint("peer", peer_.listenMultiaddr()));
  const std::string body = request("up the tree");
  EXPECT_EQ(out.roundTrip(body, 5s).value(), body) << "the echo peer returns the request";

  std::promise<bool> asyncDone;
  out.roundTripAsync(body, 5s, [&asyncDone](AmpLedgerTransport::Roe<std::string> response) {
    asyncDone.set_value(response.isOk());
  });
  auto future = asyncDone.get_future();
  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(future.get());

  node.setStop(true);
  thread.join();
}

TEST(HostedAmpRuntimeTest, NothingRunsIntoAClosedNode) {
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  Host host;
  host.start(hub, pp::adp::IpEndpoint::V4(10, 3, 0, 1, 1000));
  auto hosted = std::make_unique<network::HostedAmpRuntime>(host.forLedger());

  std::promise<bool> onIo;
  hosted->post([&]() { onIo.set_value(hosted->onIoThread()); });
  auto future = onIo.get_future();
  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(future.get()) << "a posted task runs on the host's driver thread";
  EXPECT_FALSE(hosted->onIoThread());

  // A call in progress holds the close; one after it is dropped, even queued before it.
  std::atomic<bool> ran{false};
  std::promise<void> entered;
  std::promise<void> release;
  auto guarded = hosted->guard<>(std::function<void()>([&]() {
    entered.set_value();
    release.get_future().wait();
  }));
  std::thread caller([&]() { guarded(); });
  entered.get_future().wait();
  std::atomic<bool> closed{false};
  std::thread closer([&]() {
    hosted->closeHosted();
    closed = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(closed.load()) << "close waits for the call in progress";
  release.set_value();
  caller.join();
  closer.join();
  EXPECT_TRUE(closed.load());

  auto late = hosted->guardTask([&]() { ran = true; });
  hosted->post([&]() { ran = true; });
  hosted.reset(); // the node is gone; the host runs on
  late();
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(ran.load()) << "nothing reaches a closed node";
}

TEST(HostedAmpRuntimeTest, WorkQueuedBeforeTheCloseDoesNotRunAfterIt) {
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  Host host;
  host.start(hub, pp::adp::IpEndpoint::V4(10, 4, 0, 1, 1000));
  auto hosted = std::make_unique<network::HostedAmpRuntime>(host.forLedger());

  // A call to a peer that never answers: its timeout fires on the io lane after the node closed.
  AmpLedgerTransport transport(*hosted, "nobody");
  ASSERT_TRUE(transport.registerEndpoint(
      "nobody", "/ip4/10.4.0.9/udp/9/adp/1.0.0/p2p/" + freshIdentity().local_peer_id));
  std::atomic<bool> doneCalled{false};
  transport.roundTripAsync(request("lost"), 300ms, [&](AmpLedgerTransport::Roe<std::string>) { doneCalled = true; });

  // The host's driver runs work of its own, which queues ours behind it, and holds until the node
  // closed. (Posted work runs under the host's io lock: a post from another thread would wait.)
  std::atomic<bool> ran{false};
  std::promise<void> queued;
  std::promise<void> release;
  auto released = release.get_future().share();
  host.stack->Runtime().PostToIo([&, released]() {
    hosted->post([&]() { ran = true; });
    queued.set_value();
    released.wait();
  });
  queued.get_future().wait();
  // The close shuts the gate at once, then waits for the host's turn to end (it removes its tick
  // under the host's io lock).
  std::thread closer([&]() { hosted->closeHosted(); });
  std::this_thread::sleep_for(50ms);
  release.set_value();
  closer.join();
  std::this_thread::sleep_for(600ms); // past the call's timeout
  EXPECT_FALSE(ran.load()) << "queued before the close, dropped after it";
  EXPECT_FALSE(doneCalled.load()) << "a reply (or timeout) after the close does not reach its caller";
}

// THREADING.md rule 1: a standalone node's pump claims its runtime — a drive from any other thread is
// refused — and onIoThread is the io executor's view: the pump thread, or inside its RunAndWait.
TEST(LedgerAmpRuntimeTest, ThePumpIsTheOnlyDriver) {
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  network::LedgerAmpRuntime runtime;
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, pp::adp::IpEndpoint::V4(10, 4, 0, 1, 1000));
  ASSERT_TRUE(runtime.StartForTest(io, std::make_shared<pp::adp::WallClock>(), freshIdentity()).isOk());

  std::promise<bool> onIo;
  runtime.post([&]() { onIo.set_value(runtime.onIoThread()); });
  auto future = onIo.get_future();
  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(future.get()) << "posted work runs on the pump";
  EXPECT_FALSE(runtime.onIoThread());

  const uint64_t refused = runtime.runtime().RefusedDrives();
  runtime.runtime().Drive();  // a second driver
  EXPECT_EQ(runtime.runtime().RefusedDrives(), refused + 1);

  bool inside = false;
  runtime.runtime().IoExecutor()->RunAndWait([&]() { inside = runtime.onIoThread(); });
  EXPECT_TRUE(inside) << "holding the io lock through the executor counts as on the lane";
  runtime.Stop();
}
