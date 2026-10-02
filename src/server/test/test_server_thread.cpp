#include "BlockAddPolicy.h"
#include "BlockWaitList.h"
#include "MinerRegistry.h"
#include "RequestQueue.h"
#include "Server.h"
#include "lib/common/BinaryPack.hpp"
#include "common/io/Json.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <set>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace pp;

namespace {

using namespace std::chrono_literals;

// --- RequestQueue ---

TEST(RequestQueueTest, PopsInOrderAndTimesOutWhenEmpty) {
  RequestQueue queue(4);
  ASSERT_TRUE(queue.push("", "a", [](std::string) {}));
  ASSERT_TRUE(queue.push("", "b", [](std::string) {}));
  auto first = queue.popUntil(RequestQueue::Clock::now() + 10ms);
  auto second = queue.popUntil(RequestQueue::Clock::now() + 10ms);
  ASSERT_TRUE(first && second);
  EXPECT_EQ(first->body, "a");
  EXPECT_EQ(second->body, "b");
  EXPECT_FALSE(queue.popUntil(RequestQueue::Clock::now() + 10ms));
}

TEST(RequestQueueTest, RefusesWhenFullOrClosed) {
  RequestQueue queue(1);
  ASSERT_TRUE(queue.push("", "a", [](std::string) {}));
  EXPECT_FALSE(queue.push("", "b", [](std::string) {}));
  auto pending = queue.close();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending.front().body, "a");
  EXPECT_TRUE(queue.isClosed());
  EXPECT_FALSE(queue.push("", "c", [](std::string) {}));
  EXPECT_FALSE(queue.popUntil(RequestQueue::Clock::now() + 10ms));
}

constexpr auto kLow = RequestQueue::Lane::Low;

std::string popBodies(RequestQueue& queue, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    auto item = queue.popUntil(RequestQueue::Clock::now() + 10ms);
    out += item ? item->body : "-";
  }
  return out;
}

TEST(RequestQueueTest, LowLaneIsServedAfterAStreakOfNormalWork) {
  RequestQueue queue(64);
  for (char c : std::string("ABCDEFGHIJ")) {
    ASSERT_TRUE(queue.push("", std::string(1, c), [](std::string) {}));
  }
  ASSERT_TRUE(queue.push("p", "x", [](std::string) {}, kLow));
  ASSERT_TRUE(queue.push("p", "y", [](std::string) {}, kLow));
  EXPECT_EQ(popBodies(queue, 12), "ABCDxEFGHyIJ");
}

TEST(RequestQueueTest, LowLaneAloneIsServedRightAway) {
  RequestQueue queue(64);
  ASSERT_TRUE(queue.push("p", "x", [](std::string) {}, kLow));
  EXPECT_EQ(popBodies(queue, 1), "x");
}

TEST(RequestQueueTest, ReadFloodGetsBusyWithoutCrowdingOutNormalWork) {
  RequestQueue queue(8); // Low lane holds 2
  ASSERT_TRUE(queue.push("p1", "r1", [](std::string) {}, kLow));
  ASSERT_TRUE(queue.push("p2", "r2", [](std::string) {}, kLow));
  EXPECT_FALSE(queue.push("p3", "r3", [](std::string) {}, kLow));
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(queue.push("", "n", [](std::string) {})) << i;
  }
  EXPECT_FALSE(queue.push("", "n", [](std::string) {}));
  EXPECT_EQ(queue.close().size(), 10u); // both lanes are handed back on close
}

TEST(RequestQueueTest, OnePeerCannotTakeTheWholeLowLane) {
  RequestQueue queue(1024); // Low lane holds 256
  for (size_t i = 0; i < RequestQueue::kLowPerPeer; ++i) {
    ASSERT_TRUE(queue.push("greedy", "r", [](std::string) {}, kLow));
  }
  EXPECT_FALSE(queue.push("greedy", "r", [](std::string) {}, kLow));
  EXPECT_TRUE(queue.push("other", "r", [](std::string) {}, kLow));
  ASSERT_TRUE(queue.popUntil(RequestQueue::Clock::now() + 10ms)); // one of greedy's
  EXPECT_TRUE(queue.push("greedy", "r", [](std::string) {}, kLow));
}

// --- Server: requests handled one at a time on the serving thread ---

/** Echo server; records which thread handled each request and the max overlap. */
class EchoServer : public Server {
public:
  using Server::enqueueRequest;
  using Server::postToServerThread;
  using Server::packResponse;
  using Server::serveTasksUntil;
  using Server::serveRequestsFor;
  using Server::setNetworkTuning;
  using Server::setUpstreams;
  using Server::setAllowedPeers;
  using Server::parseAllowedPeers;
  using Server::loadOrCreateIdentityKey;
  using Server::laneFor;
  using Server::setRequestLimits;
  using Server::stopAmpServer;

  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  std::atomic<int> handled{0};
  std::mutex threadsMu;
  std::vector<std::thread::id> threads;

protected:
  std::string getSignatureFileName() const override { return ".test"; }
  std::string getLogFileName() const override { return "test.log"; }
  std::string getServerName() const override { return "EchoServer"; }
  void runLoop() override {}

  /** "defer": hold the reply until the test completes it (like an outbound call). */
  bool handleDeferred(const Client::Request& request, const RequestQueue::Reply& reply) override {
    if (request.payload != "defer") {
      return false;
    }
    deferredReply = reply;
    deferredThread = std::this_thread::get_id();
    return true;
  }

public:
  RequestQueue::Reply deferredReply;
  std::thread::id deferredThread;
  /** Set to serve BLOCK_WAIT (like a beacon / relay). */
  std::optional<uint64_t> tip;

protected:
  std::optional<uint64_t> blockWaitTip() const override { return tip; }

public:

protected:
  std::string handleParsedRequest(const Client::Request& request) override {
    const int now = ++active;
    int seen = maxActive.load();
    while (now > seen && !maxActive.compare_exchange_weak(seen, now)) {
    }
    {
      std::lock_guard<std::mutex> lock(threadsMu);
      threads.push_back(std::this_thread::get_id());
    }
    if (request.payload == "throw") {
      --active;
      throw std::runtime_error("boom");
    }
    std::this_thread::sleep_for(1ms);
    ++handled;
    --active;
    return packResponse(request.payload);
  }
};

std::string packRequest(const std::string& payload, uint32_t type = 1) {
  Client::Request request;
  request.type = type;
  request.payload = payload;
  return utl::binaryPack(request);
}

/** Collects replies from any thread. */
struct Replies {
  std::mutex mu;
  std::vector<Client::Response> items;

  RequestQueue::Reply sink() {
    return [this](std::string body) {
      auto response = utl::binaryUnpack<Client::Response>(body);
      std::lock_guard<std::mutex> lock(mu);
      items.push_back(response ? *response : Client::Response{});
    };
  }
  size_t size() {
    std::lock_guard<std::mutex> lock(mu);
    return items.size();
  }
};

TEST(ServerThreadTest, HandlesRequestsFromManyThreadsOneAtATimeOnServerThread) {
  EchoServer server;
  Replies replies;
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 25;

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p]() {
      for (int i = 0; i < kPerProducer; ++i) {
        server.enqueueRequest(packRequest(std::to_string(p) + ":" + std::to_string(i)), replies.sink());
      }
    });
  }
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (replies.size() < kProducers * kPerProducer && std::chrono::steady_clock::now() < until) {
    server.serveRequestsFor(20ms);
  }
  for (auto& t : producers) {
    t.join();
  }

  ASSERT_EQ(replies.size(), static_cast<size_t>(kProducers * kPerProducer));
  EXPECT_EQ(server.handled.load(), kProducers * kPerProducer);
  EXPECT_EQ(server.maxActive.load(), 1);
  for (const auto& id : server.threads) {
    EXPECT_EQ(id, std::this_thread::get_id());
  }
  for (const auto& r : replies.items) {
    EXPECT_EQ(r.errorCode, 0);
  }
}

TEST(ServerThreadTest, RepliesBusyWhenQueueIsFull) {
  EchoServer server;
  server.setRequestLimits(2, 8000ms);
  Replies replies;
  server.enqueueRequest(packRequest("a"), replies.sink());
  server.enqueueRequest(packRequest("b"), replies.sink());
  server.enqueueRequest(packRequest("c"), replies.sink());

  ASSERT_EQ(replies.size(), 1u);  // refused at once, not queued
  EXPECT_NE(replies.items[0].errorCode, 0);
  EXPECT_NE(replies.items[0].payload.find("busy"), std::string::npos);

  server.serveRequestsFor(50ms);
  EXPECT_EQ(replies.size(), 3u);
  EXPECT_EQ(server.handled.load(), 2);
}

TEST(ServerThreadTest, NetworkTuningSizesTheRequestQueue) {
  EchoServer server;
  pp::network::NetworkTuning tuning;
  tuning.requestQueueCapacity = 1;
  server.setNetworkTuning(tuning);
  Replies replies;
  server.enqueueRequest(packRequest("a"), replies.sink());
  server.enqueueRequest(packRequest("b"), replies.sink());

  ASSERT_EQ(replies.size(), 1u);
  EXPECT_NE(replies.items[0].payload.find("busy"), std::string::npos);
  server.serveRequestsFor(50ms);
  EXPECT_EQ(server.handled.load(), 1);
}

TEST(ServerThreadTest, UpwardWritesAreRefusedFromOwnUpstream) {
  EchoServer server;
  ASSERT_TRUE(server.setUpstreams({"/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/upstream-peer"}));
  Replies replies;
  server.enqueueRequest(packRequest("from-up", Client::T_REQ_BLOCK_ADD), replies.sink(), "upstream-peer");
  server.enqueueRequest(packRequest("reg-up", Client::T_REQ_REGISTER), replies.sink(), "upstream-peer");
  server.enqueueRequest(packRequest("from-down", Client::T_REQ_BLOCK_ADD), replies.sink(), "miner-peer");
  server.enqueueRequest(packRequest("status-up", Client::T_REQ_STATUS), replies.sink(), "upstream-peer");
  server.enqueueRequest(packRequest("list-up", Client::T_REQ_MINER_LIST), replies.sink(), "upstream-peer");
  server.enqueueRequest(packRequest("tx-up", Client::T_REQ_TX_ADD), replies.sink(), "upstream-peer");
  server.enqueueRequest(packRequest("pull-up", Client::T_REQ_TX_PULL), replies.sink(), "upstream-peer");
  server.serveRequestsFor(50ms);

  ASSERT_EQ(replies.size(), 7u);
  EXPECT_NE(replies.items[5].errorCode, 0); // transactions only travel up
  EXPECT_NE(replies.items[6].errorCode, 0); // the pool lives upstream
  EXPECT_NE(replies.items[4].errorCode, 0); // the registry lives upstream
  EXPECT_NE(replies.items[0].errorCode, 0);
  EXPECT_NE(replies.items[0].payload.find("upstream"), std::string::npos);
  EXPECT_NE(replies.items[1].errorCode, 0);
  EXPECT_EQ(replies.items[2].errorCode, 0); // a downstream may submit blocks
  EXPECT_EQ(replies.items[3].errorCode, 0); // reads are fine either way
  EXPECT_EQ(server.handled.load(), 2);     // refused ones never reached a handler
}

TEST(ServerThreadTest, AllowedPeersAreServedOthersRefusedBeforeQueueing) {
  EchoServer server;
  ASSERT_TRUE(server.setUpstreams({"/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/upstream-peer"}));
  server.setAllowedPeers(std::set<std::string>{"relay-a"});
  Replies replies;
  server.enqueueRequest(packRequest("stranger"), replies.sink(), "stranger");
  server.enqueueRequest(packRequest("anonymous"), replies.sink(), "");
  ASSERT_EQ(replies.size(), 2u); // refused at once, never queued
  EXPECT_NE(replies.items[0].payload.find("not allowed"), std::string::npos);
  EXPECT_NE(replies.items[1].errorCode, 0);

  server.enqueueRequest(packRequest("listed"), replies.sink(), "relay-a");
  server.enqueueRequest(packRequest("up"), replies.sink(), "upstream-peer"); // upstreams always
  server.serveRequestsFor(50ms);
  ASSERT_EQ(replies.size(), 4u);
  EXPECT_EQ(replies.items[2].errorCode, 0);
  EXPECT_EQ(replies.items[3].errorCode, 0);
  EXPECT_EQ(server.handled.load(), 2);
}

TEST(ServerThreadTest, WithoutAllowedPeersAnyoneIsServed) {
  EchoServer server;
  Replies replies;
  server.enqueueRequest(packRequest("x"), replies.sink(), "anyone");
  server.serveRequestsFor(50ms);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies.items[0].errorCode, 0);
}

TEST(ServerThreadTest, ParseAllowedPeers) {
  auto parse = [](const std::string& json) {
    auto value = pp::common::io::valueFromJsonString(json);
    return EchoServer::parseAllowedPeers(*pp::common::asObject(value.value()));
  };
  EXPECT_FALSE(parse(R"({"port": 1})").value().has_value()); // absent: anyone
  EXPECT_EQ(parse(R"({"allowedPeers": ["a", "b"]})").value()->size(), 2u);
  EXPECT_TRUE(parse(R"({"allowedPeers": []})").value()->empty()); // only upstreams
  EXPECT_FALSE(parse(R"({"allowedPeers": "a"})").isOk());
  EXPECT_FALSE(parse(R"({"allowedPeers": [""]})").isOk());
}

// The identity key is created on first start and reloaded after; with a
// relative work dir (pp-beacon -d beacon) the path must not double up.
TEST(ServerThreadTest, IdentityKeyWithRelativeWorkDirIsCreatedThenReloaded) {
  const std::string dir = "pp-ledger-relative-identity-test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  {
    EchoServer server;
    ASSERT_TRUE(server.run(dir).isOk());
    auto first = server.loadOrCreateIdentityKey();
    ASSERT_TRUE(first.isOk()) << first.error().message;
    auto again = server.loadOrCreateIdentityKey();
    ASSERT_TRUE(again.isOk()) << again.error().message;
    EXPECT_EQ(first.value(), again.value());
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(dir) / "keys" / "amp-identity.txt"));
  }
  std::filesystem::remove_all(dir, ec);
}

TEST(ServerThreadTest, SetUpstreamsRejectsMultiaddrWithoutPeerId) {
  EchoServer server;
  EXPECT_FALSE(server.setUpstreams({"/ip4/127.0.0.1/udp/8517"}));
}

TEST(ServerThreadTest, ReadsGoToTheLowLane) {
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_BLOCK_GET), kLow);
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_TX_GET_BY_WALLET), kLow);
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_BLOCK_ADD), RequestQueue::Lane::Normal);
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_TX_PULL), RequestQueue::Lane::Normal); // leader-critical
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_CALIBRATION), RequestQueue::Lane::Normal);
}

TEST(ServerThreadTest, PeekRequestTypeMatchesThePackedRequest) {
  EXPECT_EQ(Client::peekRequestType(packRequest("payload", Client::T_REQ_BLOCK_GET)), Client::T_REQ_BLOCK_GET);
  EXPECT_EQ(Client::peekRequestType(packRequest("", 70000)), 70000u);
  EXPECT_FALSE(Client::peekRequestType("short"));
}

// --- Miner registry: renewals keep a record, silence drops it ---

Client::MinerInfo minerInfo(uint64_t id) {
  Client::MinerInfo m;
  m.id = id;
  return m;
}

TEST(MinerRegistryTest, RenewalKeepsTheRecordWithoutChangingTheVersion) {
  MinerRegistry registry;
  registry.upsert(minerInfo(1), 1000);
  const uint64_t v = registry.version();
  registry.upsert(minerInfo(1), 1060); // renewal
  EXPECT_EQ(registry.version(), v);
  EXPECT_EQ(registry.miners().at(1).tLastMessage, 1060);
  EXPECT_EQ(registry.expire(1300, 300), 0u); // renewed 240 s ago
  registry.upsert(minerInfo(2), 1310); // joined
  EXPECT_GT(registry.version(), v);
}

TEST(MinerRegistryTest, RecordsNotRenewedWithinTheTtlAreDropped) {
  MinerRegistry registry;
  registry.upsert(minerInfo(1), 1000);
  registry.upsert(minerInfo(2), 1200);
  const uint64_t v = registry.version();
  EXPECT_EQ(registry.expire(1301, 300), 1u);
  EXPECT_FALSE(registry.miners().contains(1));
  EXPECT_TRUE(registry.miners().contains(2));
  EXPECT_GT(registry.version(), v);
}

TEST(MinerRegistryTest, OnlyANewerRegistrationIsAccepted) {
  MinerRegistry registry;
  auto first = minerInfo(1);
  first.issuedAt = 500;
  EXPECT_TRUE(registry.isNewer(first));
  registry.upsert(first, 1000);
  EXPECT_FALSE(registry.isNewer(first)); // replay
  auto later = first;
  later.issuedAt = 560;
  EXPECT_TRUE(registry.isNewer(later));
}

TEST(MinerRegistryTest, SignedRecordSurvivesTheWire) {
  auto miner = minerInfo(7);
  miner.issuedAt = 1234;
  miner.signatures = {"sig-a", std::string("sig\0b", 5)};
  Client::MinerInfo back;
  ASSERT_TRUE(back.ltsFromMeta(miner.ltsToMeta()).isOk()); // as a relay re-sends it
  EXPECT_EQ(back.signatures, miner.signatures);
  EXPECT_EQ(back.signingMessage("net"), miner.signingMessage("net"));
  EXPECT_NE(back.signingMessage("net"), miner.signingMessage("other-net"));
}

// --- BLOCK_WAIT ---

TEST(BlockWaitListTest, ParksUntilTheTipPassesOrTheHoldEnds) {
  BlockWaitList waits(8);
  const auto now = BlockWaitList::Clock::now();
  int answered = 0;
  auto reply = [&](std::string) { ++answered; };
  EXPECT_EQ(waits.park("a", 5, now + 1s, reply, /*tip=*/6).size(), 1u); // already behind: answer now
  EXPECT_TRUE(waits.park("b", 6, now + 1s, reply, 6).empty());
  EXPECT_TRUE(waits.park("c", 6, now + 10ms, reply, 6).empty());
  EXPECT_TRUE(waits.poll(6, now).empty());
  EXPECT_EQ(waits.poll(6, now + 20ms).size(), 1u); // c's hold ended
  EXPECT_EQ(waits.poll(7, now + 20ms).size(), 1u); // the tip passed b
  EXPECT_EQ(waits.size(), 0u);
}

TEST(BlockWaitListTest, OneWaitPerPeerAndBounded) {
  BlockWaitList waits(1);
  const auto deadline = BlockWaitList::Clock::now() + 1s;
  auto reply = [](std::string) {};
  EXPECT_TRUE(waits.park("a", 6, deadline, reply, 6).empty());
  EXPECT_EQ(waits.park("a", 6, deadline, reply, 6).size(), 1u); // the older wait is answered
  EXPECT_EQ(waits.park("b", 6, deadline, reply, 6).size(), 1u); // full: answered at once
  EXPECT_EQ(waits.takeAll().size(), 1u);
}

uint64_t replyTip(const Client::Response& response) {
  return utl::binaryUnpack<uint64_t>(response.payload).value();
}

TEST(ServerThreadTest, BlockWaitIsAnsweredWhenTheTipMoves) {
  EchoServer server;
  server.tip = 6;
  Replies replies;
  server.enqueueRequest(packRequest(utl::binaryPack(uint64_t{4}), Client::T_REQ_BLOCK_WAIT), replies.sink(), "m1");
  server.enqueueRequest(packRequest(utl::binaryPack(uint64_t{6}), Client::T_REQ_BLOCK_WAIT), replies.sink(), "m2");
  server.serveRequestsFor(20ms);
  ASSERT_EQ(replies.size(), 1u); // m1 was behind; m2 waits
  EXPECT_EQ(replyTip(replies.items[0]), 6u);

  server.tip = 7; // a block arrived
  server.serveRequestsFor(20ms);
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_EQ(replies.items[1].errorCode, 0);
  EXPECT_EQ(replyTip(replies.items[1]), 7u);
}

TEST(ServerThreadTest, BlockWaitHoldEndsWithTheTipUnchanged) {
  EchoServer server;
  network::NetworkTuning tuning;
  tuning.rpcTimeout = 1000ms; // hold = 500 ms
  server.setNetworkTuning(tuning);
  server.tip = 6;
  Replies replies;
  server.enqueueRequest(packRequest(utl::binaryPack(uint64_t{6}), Client::T_REQ_BLOCK_WAIT), replies.sink(), "m1");
  server.serveRequestsFor(300ms);
  EXPECT_EQ(replies.size(), 0u);
  server.serveRequestsFor(400ms);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replyTip(replies.items[0]), 6u);
}

TEST(ServerThreadTest, BlockWaitIsRefusedByNodesThatDoNotServeIt) {
  EchoServer server; // a miner: no tip
  Replies replies;
  server.enqueueRequest(packRequest(utl::binaryPack(uint64_t{0}), Client::T_REQ_BLOCK_WAIT), replies.sink(), "m1");
  server.serveRequestsFor(20ms);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_NE(replies.items[0].errorCode, 0);
}

TEST(ServerThreadTest, StopAnswersParkedBlockWaits) {
  EchoServer server;
  server.tip = 6;
  Replies replies;
  server.enqueueRequest(packRequest(utl::binaryPack(uint64_t{6}), Client::T_REQ_BLOCK_WAIT), replies.sink(), "m1");
  server.serveRequestsFor(20ms);
  server.stopAmpServer();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_NE(replies.items[0].errorCode, 0);
}

TEST(ServerThreadTest, ExpiredRequestIsRefusedWithoutRunningHandler) {
  EchoServer server;
  server.setRequestLimits(8, 10ms);
  Replies replies;
  server.enqueueRequest(packRequest("stale"), replies.sink());
  std::this_thread::sleep_for(30ms);

  server.serveRequestsFor(20ms);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_NE(replies.items[0].errorCode, 0);
  EXPECT_NE(replies.items[0].payload.find("expired"), std::string::npos);
  EXPECT_EQ(server.handled.load(), 0);
}

TEST(ServerThreadTest, StopRepliesToPendingAndRefusesNew) {
  EchoServer server;
  Replies replies;
  server.enqueueRequest(packRequest("pending"), replies.sink());
  server.stopAmpServer();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_NE(replies.items[0].payload.find("stopping"), std::string::npos);

  server.enqueueRequest(packRequest("late"), replies.sink());
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_NE(replies.items[1].errorCode, 0);
  EXPECT_EQ(server.handled.load(), 0);
}

TEST(ServerThreadTest, ThrowingHandlerStillReplies) {
  EchoServer server;
  Replies replies;
  server.enqueueRequest(packRequest("throw"), replies.sink());
  server.enqueueRequest(packRequest("after"), replies.sink());
  server.serveRequestsFor(50ms);

  ASSERT_EQ(replies.size(), 2u);
  EXPECT_NE(replies.items[0].errorCode, 0);
  EXPECT_EQ(replies.items[1].errorCode, 0);
  EXPECT_EQ(replies.items[1].payload, "after");
}

// Phase 3: a handler that waits on an outbound call does not block the server
// thread; its completion comes back as a task and replies later.
TEST(ServerThreadTest, DeferredRequestRepliesFromCompletionWhileOthersAreServed) {
  EchoServer server;
  Replies replies;
  server.enqueueRequest(packRequest("defer"), replies.sink());
  server.enqueueRequest(packRequest("other"), replies.sink());
  server.serveRequestsFor(20ms);

  // "other" was served while "defer" is still outstanding.
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies.items[0].payload, "other");
  ASSERT_TRUE(server.deferredReply);
  EXPECT_EQ(server.deferredThread, std::this_thread::get_id());

  // The "outbound call" completes on another thread and hops back.
  std::thread::id completionThread;
  std::thread io([&]() {
    server.postToServerThread([&]() {
      completionThread = std::this_thread::get_id();
      server.deferredReply(EchoServer::packResponse("deferred-done"));
    });
  });
  io.join();
  server.serveRequestsFor(20ms);

  ASSERT_EQ(replies.size(), 2u);
  EXPECT_EQ(replies.items[1].payload, "deferred-done");
  EXPECT_EQ(completionThread, std::this_thread::get_id());
}

TEST(ServerThreadTest, CompletionAfterStopIsDropped) {
  EchoServer server;
  server.stopAmpServer();
  bool ran = false;
  server.postToServerThread([&]() { ran = true; });
  server.serveRequestsFor(20ms);
  EXPECT_FALSE(ran);
}

} // namespace


// --- Block add check, transaction pool ---

#include "TxPool.h"

namespace {

/** Holds blocks 0..n-1; a block's hash is its slot number as text. */
struct FakeChain {
  std::vector<Ledger::ChainNode> blocks;
  uint64_t getNextBlockId() const { return blocks.size(); }
  Roe<Ledger::ChainNode> readBlock(uint64_t id) const {
    if (id >= blocks.size()) {
      return Error(1, "no block");
    }
    return blocks[id];
  }
  std::string calculateHash(const Ledger::Block& block) const { return std::to_string(block.slot); }
};

Ledger::ChainNode makeBlock(uint64_t index, uint64_t slot) {
  Ledger::ChainNode node;
  node.block.index = index;
  node.block.slot = slot;
  node.hash = std::to_string(slot);
  return node;
}

TEST(BlockAddPolicyTest, RepeatSucceedsConflictIsRefusedTipIsNew) {
  FakeChain chain;
  chain.blocks = {makeBlock(0, 0), makeBlock(1, 5)};
  EXPECT_EQ(checkBlockAdd(chain, makeBlock(1, 5)), BlockAddCheck::AlreadyHave);
  EXPECT_EQ(checkBlockAdd(chain, makeBlock(1, 6)), BlockAddCheck::Conflicts);
  EXPECT_EQ(checkBlockAdd(chain, makeBlock(2, 7)), BlockAddCheck::New);
  EXPECT_EQ(checkBlockAdd(chain, makeBlock(9, 9)), BlockAddCheck::New); // addBlock reports the gap
}

Ledger::Record transfer(uint64_t idempotentId, int64_t validUntil) {
  Ledger::TxDefault tx;
  tx.tokenId = 0;
  tx.fromWalletId = (1ULL << 30);
  tx.toWalletId = (1ULL << 30) + 1;
  tx.amount = 5;
  tx.fee = 1;
  tx.idempotentId = idempotentId;
  tx.validationTsMax = validUntil;
  Ledger::Record r;
  r.type = Ledger::T_DEFAULT;
  r.data = utl::binaryPack(tx);
  r.signatures = {"sig"};
  return r;
}

TEST(TxPoolTest, DuplicatesAreHeldOnceAndIncludedOnesLeave) {
  TxPool pool;
  auto a = transfer(1, 2000), b = transfer(2, 2000);
  EXPECT_EQ(pool.add(a, 1000), TxPool::Add::Added);
  EXPECT_EQ(pool.add(a, 1000), TxPool::Add::Duplicate);
  EXPECT_EQ(pool.add(b, 1000), TxPool::Add::Added);
  ASSERT_EQ(pool.pending(10).size(), 2u);
  EXPECT_EQ(pool.pending(1).front().data, a.data); // oldest first

  pool.removeIncluded({a}); // a committed block included it
  ASSERT_EQ(pool.size(), 1u);
  EXPECT_EQ(pool.pending(10).front().data, b.data);
}

TEST(TxPoolTest, ExpiresAtValidityWindowOrTtl) {
  TxPool::Limits limits;
  limits.ttlSeconds = 600;
  TxPool pool(limits);
  pool.add(transfer(1, /*validUntil=*/1100), 1000); // its own window ends first
  pool.add(transfer(2, /*validUntil=*/0), 1000);    // no window: TTL
  EXPECT_EQ(pool.expire(1100), 0u);
  EXPECT_EQ(pool.expire(1101), 1u);
  EXPECT_EQ(pool.expire(1600), 0u);
  EXPECT_EQ(pool.expire(1601), 1u);
  EXPECT_EQ(pool.size(), 0u);
}

TEST(TxPoolTest, FullPoolRefuses) {
  TxPool::Limits limits;
  limits.maxRecords = 1;
  TxPool pool(limits);
  EXPECT_EQ(pool.add(transfer(1, 2000), 1000), TxPool::Add::Added);
  EXPECT_EQ(pool.add(transfer(2, 2000), 1000), TxPool::Add::Full);
}

} // namespace

namespace {

TEST(ServerThreadTest, ServeTasksUntilRunsCompletionsAndLeavesRequestsQueued) {
  EchoServer server;
  Replies replies;
  server.enqueueRequest(packRequest("queued-request"), replies.sink());
  bool done = false;
  std::thread io([&]() { server.postToServerThread([&]() { done = true; }); });
  io.join();
  EXPECT_TRUE(server.serveTasksUntil([&]() { return done; }, std::chrono::seconds(2)));
  EXPECT_EQ(replies.size(), 0u);  // the request was not served
  server.serveRequestsFor(std::chrono::milliseconds(20));
  EXPECT_EQ(replies.size(), 1u);
}

// onStart runs while Service's stop flag is still set (it is cleared after).
TEST(ServerThreadTest, ServeTasksUntilWorksWhileStopFlagIsSet) {
  EchoServer server;
  server.setStop(true);
  bool done = false;
  std::thread io([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    server.postToServerThread([&]() { done = true; });
  });
  EXPECT_TRUE(server.serveTasksUntil([&]() { return done; }, std::chrono::seconds(2)));
  io.join();
}

TEST(ServerThreadTest, ServeTasksUntilTimesOut) {
  EchoServer server;
  EXPECT_FALSE(server.serveTasksUntil([]() { return false; }, std::chrono::milliseconds(30)));
}

} // namespace
