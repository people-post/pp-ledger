#include "BlockAddPolicy.h"
#include "BlockWaitList.h"
#include "MinerRegistry.h"
#include "RequestQueue.h"
#include "Server.h"
#include "lib/common/BinaryPack.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
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
  server.serveRequestsFor(50ms);

  ASSERT_EQ(replies.size(), 5u);
  EXPECT_NE(replies.items[4].errorCode, 0); // the registry lives upstream
  EXPECT_NE(replies.items[0].errorCode, 0);
  EXPECT_NE(replies.items[0].payload.find("upstream"), std::string::npos);
  EXPECT_NE(replies.items[1].errorCode, 0);
  EXPECT_EQ(replies.items[2].errorCode, 0); // a downstream may submit blocks
  EXPECT_EQ(replies.items[3].errorCode, 0); // reads are fine either way
  EXPECT_EQ(server.handled.load(), 2);     // refused ones never reached a handler
}

TEST(ServerThreadTest, SetUpstreamsRejectsMultiaddrWithoutPeerId) {
  EchoServer server;
  EXPECT_FALSE(server.setUpstreams({"/ip4/127.0.0.1/udp/8517"}));
}

TEST(ServerThreadTest, ReadsGoToTheLowLane) {
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_BLOCK_GET), kLow);
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_TX_GET_BY_WALLET), kLow);
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_BLOCK_ADD), RequestQueue::Lane::Normal);
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_TX_FORWARD), RequestQueue::Lane::Normal);
  EXPECT_EQ(EchoServer::laneFor(Client::T_REQ_CALIBRATION), RequestQueue::Lane::Normal);
}

TEST(ServerThreadTest, PeekRequestTypeMatchesThePackedRequest) {
  EXPECT_EQ(Client::peekRequestType(packRequest("payload", Client::T_REQ_BLOCK_GET)), Client::T_REQ_BLOCK_GET);
  EXPECT_EQ(Client::peekRequestType(packRequest("", 70000)), 70000u);
  EXPECT_FALSE(Client::peekRequestType("short"));
}

// --- Miner registry: renewals keep a record, silence drops it ---

Client::MinerInfo minerInfo(uint64_t id, const std::string& endpoint) {
  Client::MinerInfo m;
  m.id = id;
  m.endpoint = endpoint;
  return m;
}

TEST(MinerRegistryTest, RenewalKeepsTheRecordWithoutChangingTheVersion) {
  MinerRegistry registry;
  registry.upsert(minerInfo(1, "/a"), 1000);
  const uint64_t v = registry.version();
  registry.upsert(minerInfo(1, "/a"), 1060); // renewal
  EXPECT_EQ(registry.version(), v);
  EXPECT_EQ(registry.miners().at(1).tLastMessage, 1060);
  EXPECT_EQ(registry.expire(1300, 300), 0u); // renewed 240 s ago
  registry.upsert(minerInfo(1, "/b"), 1310); // moved
  EXPECT_GT(registry.version(), v);
}

TEST(MinerRegistryTest, RecordsNotRenewedWithinTheTtlAreDropped) {
  MinerRegistry registry;
  registry.upsert(minerInfo(1, "/a"), 1000);
  registry.upsert(minerInfo(2, "/b"), 1200);
  const uint64_t v = registry.version();
  EXPECT_EQ(registry.expire(1301, 300), 1u);
  EXPECT_FALSE(registry.miners().contains(1));
  EXPECT_TRUE(registry.miners().contains(2));
  EXPECT_GT(registry.version(), v);
}

TEST(MinerRegistryTest, OnlyANewerRegistrationIsAccepted) {
  MinerRegistry registry;
  auto first = minerInfo(1, "/a");
  first.issuedAt = 500;
  EXPECT_TRUE(registry.isNewer(first));
  registry.upsert(first, 1000);
  EXPECT_FALSE(registry.isNewer(first)); // replay
  auto later = first;
  later.issuedAt = 560;
  EXPECT_TRUE(registry.isNewer(later));
}

TEST(MinerRegistryTest, SignedRecordSurvivesTheWire) {
  auto miner = minerInfo(7, "/ip4/127.0.0.1/udp/9000/adp/1.0.0/p2p/x");
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


// --- Transaction forward cap (receiver rule) ---

#include "TxForwardPolicy.h"

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

TEST(TxForwardPolicyTest, LeaderOfCurrentSlotPoolsAndOfUpcomingSlotHolds) {
  EXPECT_EQ(decideTxForward(3, 3, 40, 40, true), TxForwardAction::AddToPool);
  EXPECT_EQ(decideTxForward(3, 3, 40, 41, true), TxForwardAction::HoldForSlot);
}

// Clock skew at a slot boundary: A (still in slot 40) forwards to B, the leader
// it sees for slot 40; B is already in slot 41. B must not send it back.
TEST(TxForwardPolicyTest, ReceiverAheadOfTargetSlotCachesInsteadOfBouncing) {
  EXPECT_EQ(decideTxForward(3, 3, /*currentSlot=*/41, /*targetSlot=*/40, /*leads=*/true),
            TxForwardAction::CacheNotLeader);
  EXPECT_EQ(decideTxForward(3, 3, 41, 40, false), TxForwardAction::CacheNotLeader);
}

TEST(TxForwardPolicyTest, NotLeaderCaches) {
  EXPECT_EQ(decideTxForward(3, 3, 40, 40, false), TxForwardAction::CacheNotLeader);
  EXPECT_EQ(decideTxForward(3, 3, 40, 42, false), TxForwardAction::CacheNotLeader);
}

// A receiver whose tip is in an earlier epoch may be using a provisional leader
// schedule; it must not act on it (not even to pool), only cache and sync.
TEST(TxForwardPolicyTest, ReceiverBehindSenderEpochCachesEvenIfItThinksItLeads) {
  EXPECT_EQ(decideTxForward(/*tipEpoch=*/2, /*senderTipEpoch=*/3, 40, 40, true), TxForwardAction::CacheBehind);
  EXPECT_EQ(decideTxForward(2, 3, 40, 40, false), TxForwardAction::CacheBehind);
  // Ahead of the sender is fine.
  EXPECT_EQ(decideTxForward(4, 3, 40, 40, true), TxForwardAction::AddToPool);
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
