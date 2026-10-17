#include "Client.h"
#include "FollowerServer.h"
#include "InProcessLedgerTransport.h"
#include "Server.h"
#include "lib/common/BinaryPack.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

using namespace pp;
using namespace std::chrono_literals;

namespace {

// LR006 / LR007: version 2 carries the deadline and the retry-after; version 1 is still read.
TEST(OverloadTest, EnvelopesOfBothVersionsAreRead) {
  Client::Request v1;
  v1.version = 1;
  v1.type = Client::T_REQ_STATUS;
  v1.payload = "p";
  v1.deadlineMs = 777; // not on the wire in version 1
  auto request = utl::binaryUnpack<Client::Request>(utl::binaryPack(v1));
  ASSERT_TRUE(request) << request.error().message;
  EXPECT_EQ(request.value().payload, "p");
  EXPECT_EQ(request.value().deadlineMs, 0u);

  Client::Request v2;
  v2.type = Client::T_REQ_TX_ADD;
  v2.deadlineMs = 30000;
  request = utl::binaryUnpack<Client::Request>(utl::binaryPack(v2));
  ASSERT_TRUE(request);
  EXPECT_EQ(request.value().deadlineMs, 30000u);

  Client::Response old;
  old.version = 1;
  old.errorCode = 3;
  old.payload = "an error";
  auto response = utl::binaryUnpack<Client::Response>(utl::binaryPack(old));
  ASSERT_TRUE(response);
  EXPECT_EQ(response.value().retryAfterMs, 0u);
  Client::Response busy;
  busy.errorCode = Client::E_BUSY;
  busy.retryAfterMs = 1500;
  response = utl::binaryUnpack<Client::Response>(utl::binaryPack(busy));
  ASSERT_TRUE(response);
  EXPECT_EQ(response.value().retryAfterMs, 1500u);
}

/** A node scripted reply by reply; it records each request it got. */
struct ScriptedNode {
  std::vector<std::string> replies; ///< packed Responses, in turn (the last repeats)
  std::vector<Client::Request> got;

  std::unique_ptr<ILedgerTransport> transport() {
    return std::make_unique<InProcessLedgerTransport>([this](const std::string &body) {
      auto request = utl::binaryUnpack<Client::Request>(body);
      got.push_back(request ? request.value() : Client::Request{});
      return replies[std::min(got.size(), replies.size()) - 1];
    });
  }
};

std::string reply(uint16_t code, const std::string &payload, uint32_t retryAfterMs = 0) {
  Client::Response r;
  r.errorCode = code;
  r.payload = payload;
  r.retryAfterMs = retryAfterMs;
  return utl::binaryPack(r);
}

Ledger::Record someRecord() {
  Ledger::Record record;
  record.type = Ledger::T_DEFAULT;
  Ledger::TxDefault tx;
  tx.fromWalletId = (1ULL << 30) + 1;
  tx.toWalletId = (1ULL << 30) + 2;
  tx.amount = 1;
  tx.idempotentId = 9;
  tx.validationTsMax = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count() +
                       3600;
  record.data = utl::binaryPack(tx);
  record.signatures = {"sig"};
  return record;
}

// LR007: the client sees the node's code and when to try again.
TEST(OverloadTest, TheClientSeesTheCodeAndTheRetryAfter) {
  ScriptedNode node;
  node.replies = {reply(Client::E_POOL_FULL, "Transaction pool full, please retry later", 1234)};
  Client client;
  client.setTransport(node.transport());
  auto result = client.addTransaction(someRecord());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, Client::E_POOL_FULL);
  EXPECT_EQ(result.error().retryAfterMs, 1234u);
  EXPECT_TRUE(Client::isRetryable(result.error()));
  ASSERT_EQ(node.got.size(), 1u);
  EXPECT_GT(node.got[0].deadlineMs, 0u) << "every request says how long its client waits (LR006)";
}

// LR007: one retry policy — the same bytes again after an overload code, until taken.
TEST(OverloadTest, SubmitSendsTheSameBytesAgainUntilTaken) {
  ScriptedNode node;
  node.replies = {reply(Client::E_BUSY, "Server busy, please retry", 20),
                  reply(Client::E_UPSTREAM_TIMEOUT, "The upstream did not answer in time", 20),
                  reply(0, "Transaction submitted")};
  Client client;
  client.setTransport(node.transport());
  auto result = client.submitTransaction(someRecord(), 10s);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result.value(), "Transaction submitted");
  ASSERT_EQ(node.got.size(), 3u);
  EXPECT_EQ(node.got[1].payload, node.got[0].payload) << "never re-signed";
  EXPECT_EQ(node.got[2].payload, node.got[0].payload);
}

TEST(OverloadTest, SubmitStopsAtARefusalOrWhenItsTimeIsUp) {
  ScriptedNode refusing;
  refusing.replies = {reply(1, "Transaction refused: Invalid or duplicate signature")};
  Client client;
  client.setTransport(refusing.transport());
  EXPECT_FALSE(client.submitTransaction(someRecord(), 10s));
  EXPECT_EQ(refusing.got.size(), 1u) << "a refusal is final";

  ScriptedNode full;
  full.replies = {reply(Client::E_POOL_FULL, "full", 50)};
  client.setTransport(full.transport());
  const auto start = std::chrono::steady_clock::now();
  auto result = client.submitTransaction(someRecord(), 300ms);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, Client::E_POOL_FULL);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_GE(full.got.size(), 2u);
}

struct Peek : FollowerServer {
  using FollowerServer::forwardBudget;
  using FollowerServer::kMinForward;
};

// LR006: a relay forwards with the client's remaining deadline, less a margin to answer in.
TEST(OverloadTest, ARelayForwardsWithWhatRemainsOfTheDeadline) {
  Client::Request request;
  request.deadlineMs = 30000;
  EXPECT_EQ(Peek::forwardBudget(request), 28000ms) << "a tenth, at most 2 s";
  request.deadlineMs = 1000;
  EXPECT_EQ(Peek::forwardBudget(request), 750ms) << "at least 250 ms";
  request.deadlineMs = 200;
  EXPECT_LT(Peek::forwardBudget(request), Peek::kMinForward) << "no time left: not forwarded";
  request.deadlineMs = 0; // an old client: half the default
  EXPECT_EQ(Peek::forwardBudget(request), 15000ms);
}

} // namespace
