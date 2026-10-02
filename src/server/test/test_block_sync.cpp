#include "BlockSync.h"
#include "lib/common/BinaryPack.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace pp;

namespace {

/**
 * Fake upstream: holds each request until the test completes it, so tests can
 * reorder or fail responses. Serves calibration and block gets from `chain`.
 */
class FakeUpstream : public ILedgerTransport {
public:
  std::vector<Ledger::ChainNode> chain;
  std::deque<std::pair<Client::Request, Done>> pending;

  Roe<std::string> roundTrip(const std::string &, std::chrono::milliseconds) override {
    return LedgerTransportError(-1, "sync path must be async");
  }
  void roundTripAsync(const std::string &body, std::chrono::milliseconds, Done done) override {
    auto request = utl::binaryUnpack<Client::Request>(body);
    ASSERT_TRUE(request.isOk());
    pending.emplace_back(request.value(), std::move(done));
  }

  /** Answer the request at `index` (in arrival order); fail it if `fail`. */
  void complete(size_t index, bool fail = false) {
    auto [request, done] = std::move(pending[index]);
    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
    if (fail) {
      done(LedgerTransportError(-1, "upstream unavailable"));
      return;
    }
    Client::Response response;
    if (request.type == Client::T_REQ_CALIBRATION) {
      Client::CalibrationResponse cal;
      cal.nextBlockId = chain.size();
      response.payload = utl::binaryPack(cal);
    } else {
      const uint64_t id = utl::binaryUnpack<uint64_t>(request.payload).value();
      response.payload = chain.at(id).ltsToString();
    }
    done(utl::binaryPack(response));
  }
  void completeAll() {
    while (!pending.empty()) {
      complete(0);
    }
  }
};

Ledger::ChainNode makeBlock(uint64_t index) {
  Ledger::ChainNode node;
  node.block.index = index;
  node.hash = "h" + std::to_string(index);
  return node;
}

class BlockSyncTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (uint64_t i = 0; i < 10; ++i) {
      upstream_->chain.push_back(makeBlock(i));
    }
    for (uint64_t i = 0; i < 3; ++i) {
      local_.push_back(makeBlock(i));
    }
    client_.setTransport(std::unique_ptr<ILedgerTransport>(upstream_));
  }

  BlockSync::Hooks hooks() {
    BlockSync::Hooks h;
    h.dialUpstream = []() { return pp::Roe<void>(); };
    h.nextBlockId = [this]() { return static_cast<uint64_t>(local_.size()); };
    h.applyBlock = [this](const Ledger::ChainNode &block) -> pp::Roe<void> {
      if (block.block.index != local_.size()) {
        return pp::Error("out of order: " + std::to_string(block.block.index));
      }
      if (block.block.index == failApplyAt_) {
        return pp::Error("invalid block");
      }
      local_.push_back(block);
      return {};
    };
    h.postToServerThread = [this](std::function<void()> task) { tasks_.push_back(std::move(task)); };
    h.onFinished = [this](const BlockSync::Result &r) { results_.push_back(r); };
    return h;
  }

  /** Run posted tasks as the server thread would. */
  void drain() {
    while (!tasks_.empty()) {
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      task();
    }
  }

  FakeUpstream *upstream_ = new FakeUpstream();  // owned by client_
  Client client_;
  std::vector<Ledger::ChainNode> local_;
  std::deque<std::function<void()>> tasks_;
  std::vector<BlockSync::Result> results_;
  uint64_t failApplyAt_ = UINT64_MAX;
};

TEST_F(BlockSyncTest, FetchesGapWithBoundedPipelineAndAppliesInOrder) {
  BlockSync sync(client_, hooks());
  ASSERT_EQ(sync.start(), BlockSync::Start::Started);
  EXPECT_EQ(sync.start(), BlockSync::Start::AlreadyRunning);
  upstream_->complete(0);  // calibration
  drain();
  EXPECT_EQ(upstream_->pending.size(), 4u);  // window of 4 block gets
  while (!upstream_->pending.empty() || !tasks_.empty()) {
    upstream_->completeAll();
    drain();
  }
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(results_[0].blocksAdded, 7u);
  EXPECT_EQ(local_.size(), 10u);
  EXPECT_FALSE(sync.inFlight());
}

TEST_F(BlockSyncTest, OutOfOrderResponsesStillApplyInOrder) {
  BlockSync sync(client_, hooks());
  sync.start();
  upstream_->complete(0);
  drain();
  // Answer the window newest-first.
  for (size_t i = upstream_->pending.size(); i-- > 0;) {
    upstream_->complete(i);
  }
  drain();
  EXPECT_EQ(local_.size(), 7u);  // 3..6 applied in order
  while (!upstream_->pending.empty() || !tasks_.empty()) {
    upstream_->completeAll();
    drain();
  }
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok);
  EXPECT_EQ(local_.size(), 10u);
}

TEST_F(BlockSyncTest, AlreadyInSyncFinishesWithoutFetching) {
  upstream_->chain.resize(3);
  BlockSync sync(client_, hooks());
  sync.start();
  upstream_->complete(0);
  drain();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok);
  EXPECT_EQ(results_[0].blocksAdded, 0u);
  EXPECT_TRUE(upstream_->pending.empty());
}

TEST_F(BlockSyncTest, FailureBacksOffAndIgnoresLateResults) {
  BlockSync::Options options;
  options.initialBackoff = std::chrono::milliseconds(50);
  BlockSync sync(client_, hooks(), options);
  sync.start();
  upstream_->complete(0);
  drain();
  upstream_->complete(0, /*fail=*/true);  // first block get fails
  drain();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_FALSE(sync.inFlight());
  // Late answers for the failed sync change nothing.
  upstream_->completeAll();
  drain();
  EXPECT_EQ(local_.size(), 3u);
  EXPECT_EQ(results_.size(), 1u);

  EXPECT_EQ(sync.start(), BlockSync::Start::BackingOff);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  ASSERT_EQ(sync.start(), BlockSync::Start::Started);
  while (!upstream_->pending.empty() || !tasks_.empty()) {
    upstream_->completeAll();
    drain();
  }
  ASSERT_EQ(results_.size(), 2u);
  EXPECT_TRUE(results_[1].ok);
  EXPECT_EQ(local_.size(), 10u);
}

TEST_F(BlockSyncTest, InvalidBlockFailsTheSync) {
  failApplyAt_ = 5;
  BlockSync sync(client_, hooks());
  sync.start();
  while (!upstream_->pending.empty() || !tasks_.empty()) {
    upstream_->completeAll();
    drain();
  }
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_EQ(local_.size(), 5u);
}

// The tip can move by another path (e.g. our own block) while a sync runs.
TEST_F(BlockSyncTest, BlocksAlreadyAppliedElsewhereAreSkipped) {
  BlockSync sync(client_, hooks());
  sync.start();
  upstream_->complete(0);
  drain();
  local_.push_back(makeBlock(3));
  local_.push_back(makeBlock(4));
  while (!upstream_->pending.empty() || !tasks_.empty()) {
    upstream_->completeAll();
    drain();
  }
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 10u);
}

TEST_F(BlockSyncTest, DestroyedSyncDropsLateResults) {
  {
    BlockSync sync(client_, hooks());
    sync.start();
  }
  upstream_->completeAll();
  drain();
  EXPECT_TRUE(results_.empty());
  EXPECT_TRUE(tasks_.empty());
}

} // namespace

#include "BroadcastTally.h"

namespace {

TEST(BroadcastTallyTest, CommitsOnFirstSuccessOnly) {
  BroadcastTally tally(3);
  EXPECT_EQ(tally.onResult(false), BroadcastTally::Event::None);
  EXPECT_EQ(tally.onResult(true), BroadcastTally::Event::Commit);
  EXPECT_EQ(tally.onResult(true), BroadcastTally::Event::None);
  EXPECT_TRUE(tally.done());
  EXPECT_TRUE(tally.committed());
}

TEST(BroadcastTallyTest, AllFailedOnlyAfterTheLastResult) {
  BroadcastTally tally(2);
  EXPECT_EQ(tally.onResult(false), BroadcastTally::Event::None);
  EXPECT_FALSE(tally.done());
  EXPECT_EQ(tally.onResult(false), BroadcastTally::Event::AllFailed);
  EXPECT_TRUE(tally.done());
  EXPECT_FALSE(tally.committed());
}

TEST(BroadcastTallyTest, FailureAfterCommitIsNotAllFailed) {
  BroadcastTally tally(2);
  EXPECT_EQ(tally.onResult(true), BroadcastTally::Event::Commit);
  EXPECT_EQ(tally.onResult(false), BroadcastTally::Event::None);
}

TEST(BroadcastTallyTest, SingleUpstream) {
  BroadcastTally ok(1);
  EXPECT_EQ(ok.onResult(true), BroadcastTally::Event::Commit);
  BroadcastTally failed(1);
  EXPECT_EQ(failed.onResult(false), BroadcastTally::Event::AllFailed);
}

} // namespace

#include "Server.h"

namespace {

/** Answers at once; fails the first `failures` requests. */
class FlakyUpstream : public ILedgerTransport {
public:
  std::vector<Ledger::ChainNode> chain;
  int failures = 0;

  Roe<std::string> roundTrip(const std::string &, std::chrono::milliseconds) override {
    return LedgerTransportError(-1, "async only");
  }
  void roundTripAsync(const std::string &body, std::chrono::milliseconds, Done done) override {
    if (failures > 0) {
      --failures;
      done(LedgerTransportError(-1, "upstream unavailable"));
      return;
    }
    auto request = utl::binaryUnpack<Client::Request>(body).value();
    Client::Response response;
    if (request.type == Client::T_REQ_CALIBRATION) {
      Client::CalibrationResponse cal;
      cal.nextBlockId = chain.size();
      response.payload = utl::binaryPack(cal);
    } else {
      response.payload = chain.at(utl::binaryUnpack<uint64_t>(request.payload).value()).ltsToString();
    }
    done(utl::binaryPack(response));
  }
};

class StartupServer : public Server {
public:
  using Server::postToServerThread;
  using Server::runStartupSync;

protected:
  std::string getSignatureFileName() const override { return ".test"; }
  std::string getLogFileName() const override { return "test.log"; }
  std::string getServerName() const override { return "StartupServer"; }
  void runLoop() override {}
  std::string handleParsedRequest(const Client::Request &) override { return {}; }
};

class StartupSyncTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (uint64_t i = 0; i < 6; ++i) {
      upstream_->chain.push_back(makeBlock(i));
    }
    client_.setTransport(std::unique_ptr<ILedgerTransport>(upstream_));
    BlockSync::Hooks h;
    h.dialUpstream = []() { return pp::Roe<void>(); };
    h.nextBlockId = [this]() { return static_cast<uint64_t>(local_.size()); };
    h.applyBlock = [this](const Ledger::ChainNode &block) -> pp::Roe<void> {
      local_.push_back(block);
      return {};
    };
    h.postToServerThread = [this](std::function<void()> task) { server_.postToServerThread(std::move(task)); };
    h.onFinished = [this](const BlockSync::Result &r) { last_ = r; };
    BlockSync::Options options;
    options.initialBackoff = std::chrono::milliseconds(20);
    options.maxBackoff = std::chrono::milliseconds(40);
    sync_ = std::make_unique<BlockSync>(client_, std::move(h), options);
  }

  FlakyUpstream *upstream_ = new FlakyUpstream();  // owned by client_
  Client client_;
  StartupServer server_;
  std::vector<Ledger::ChainNode> local_;
  std::optional<BlockSync::Result> last_;
  std::unique_ptr<BlockSync> sync_;
};

// One transient failure must not kill a node at startup.
TEST_F(StartupSyncTest, RetriesUntilSuccess) {
  upstream_->failures = 3;
  server_.setStop(true);  // as during onStart
  EXPECT_EQ(server_.runStartupSync(*sync_, last_, std::chrono::seconds(5)), "");
  EXPECT_EQ(local_.size(), 6u);
}

TEST_F(StartupSyncTest, GivesUpWithTheLastErrorAtTimeout) {
  upstream_->failures = 1000000;
  const std::string error = server_.runStartupSync(*sync_, last_, std::chrono::milliseconds(200));
  EXPECT_NE(error.find("upstream unavailable"), std::string::npos) << error;
  EXPECT_TRUE(local_.empty());
}

} // namespace
