#include "RequestQueue.h"
#include "Server.h"
#include "lib/common/BinaryPack.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
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
  ASSERT_TRUE(queue.push("a", [](std::string) {}));
  ASSERT_TRUE(queue.push("b", [](std::string) {}));
  auto first = queue.popUntil(RequestQueue::Clock::now() + 10ms);
  auto second = queue.popUntil(RequestQueue::Clock::now() + 10ms);
  ASSERT_TRUE(first && second);
  EXPECT_EQ(first->body, "a");
  EXPECT_EQ(second->body, "b");
  EXPECT_FALSE(queue.popUntil(RequestQueue::Clock::now() + 10ms));
}

TEST(RequestQueueTest, RefusesWhenFullOrClosed) {
  RequestQueue queue(1);
  ASSERT_TRUE(queue.push("a", [](std::string) {}));
  EXPECT_FALSE(queue.push("b", [](std::string) {}));
  auto pending = queue.close();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending.front().body, "a");
  EXPECT_TRUE(queue.isClosed());
  EXPECT_FALSE(queue.push("c", [](std::string) {}));
  EXPECT_FALSE(queue.popUntil(RequestQueue::Clock::now() + 10ms));
}

// --- Server: requests handled one at a time on the serving thread ---

/** Echo server; records which thread handled each request and the max overlap. */
class EchoServer : public Server {
public:
  using Server::enqueueRequest;
  using Server::postToServerThread;
  using Server::packResponse;
  using Server::serveRequestsFor;
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

std::string packRequest(const std::string& payload) {
  Client::Request request;
  request.type = 1;
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
