#include "BlockSync.h"
#include "UpstreamTipWatch.h"
#include "../chain/BlockValidation.h"
#include "lib/common/BinaryPack.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <map>
#include <optional>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace pp;

namespace {

/**
 * A block whose hash is real and whose previousHash links it to `previous` (empty for the first).
 */
Ledger::ChainNode makeBlock(uint64_t index, const std::string &previousHash = "") {
  Ledger::ChainNode node;
  node.block.index = index;
  node.block.previousHash = previousHash;
  node.hash = chain_block::calculateBlockHash(node.block);
  return node;
}

/** `length` linked blocks; `records` puts records in some of them. */
std::vector<Ledger::ChainNode> makeChain(size_t length, const std::map<uint64_t, std::vector<Ledger::Record>> &records = {}) {
  std::vector<Ledger::ChainNode> chain;
  for (uint64_t i = 0; i < length; ++i) {
    Ledger::ChainNode node;
    node.block.index = i;
    node.block.previousHash = chain.empty() ? std::string() : chain.back().hash;
    if (auto it = records.find(i); it != records.end()) {
      node.block.records = it->second;
    }
    node.hash = chain_block::calculateBlockHash(node.block);
    chain.push_back(node);
  }
  return chain;
}

/** The generation-1 beacon key of the test chain. */
const utl::MlDsaKeyPair &beaconKey() {
  static const utl::MlDsaKeyPair key = utl::mlDsaGenerate().value();
  return key;
}

/** A tip over `block`, signed by `key` (the test beacon's unless given). */
Client::SignedTip tipFor(const Ledger::ChainNode &block, const utl::MlDsaKeyPair &key = beaconKey(),
                         uint64_t generation = 1) {
  Client::SignedTip tip;
  tip.networkId = "net";
  tip.generation = generation;
  tip.height = block.block.index;
  tip.blockHash = block.hash;
  tip.signature = utl::mlDsaSign(key.privateKey, tip.signingMessage()).value();
  return tip;
}

/** The test chain's admin set: one key. */
const utl::MlDsaKeyPair &adminKey() {
  static const utl::MlDsaKeyPair key = utl::mlDsaGenerate().value();
  return key;
}

/** The authority at the local tip of a test node: generation 1 is beaconKey(). */
pp::Roe<AuthorityTrail> testTrail() {
  ChainAuthority authority;
  authority.beacon.networkId = "net";
  authority.beacon.generation = 1;
  authority.beacon.publicKey = beaconKey().publicKey;
  Client::Wallet admin;
  admin.publicKeys = {adminKey().publicKey};
  admin.minSignatures = 1;
  admin.keyType = Crypto::TK_ML_DSA_65;
  return AuthorityTrail("net", authority, admin);
}

/** A T_BEACON_CERT for `generation` with `key`, signed by `signer` (the admin set's key unless given). */
Ledger::Record certRecord(uint64_t generation, const utl::MlDsaKeyPair &key, uint64_t cutoverHeight,
                          const utl::MlDsaKeyPair &signer = adminKey()) {
  BeaconCertificate cert;
  cert.networkId = "net";
  cert.generation = generation;
  cert.publicKey = key.publicKey;
  cert.cutoverHeight = cutoverHeight;
  Ledger::TxBeaconCert tx;
  tx.meta = cert.ltsToString();
  Ledger::Record record;
  record.type = Ledger::T_BEACON_CERT;
  record.data = utl::binaryPack(tx);
  record.signatures = {utl::mlDsaSign(signer.privateKey, record.signingMessage("net")).value()};
  return record;
}

/** STATUS as an upstream serves it: its next block id and signed tip. */
std::string statusPayload(const std::vector<Ledger::ChainNode> &chain, const std::optional<Client::SignedTip> &tip) {
  Client::BeaconState state;
  state.nextBlockId = chain.size();
  state.signedTip = tip;
  return utl::binaryPack(state.ltsToMeta());
}

/**
 * Fake upstream: holds each request until the test completes it, so tests can
 * reorder or fail responses. Serves STATUS and block gets from `chain`.
 */
class FakeUpstream : public ILedgerTransport {
public:
  std::vector<Ledger::ChainNode> chain;
  /** Sent with BLOCK_WAIT replies, as a beacon (or relay) does; STATUS too when set. */
  std::optional<Client::SignedTip> signedTip;
  /** STATUS without `signedTip`: sign the last block of `chain` (false: no tip at all). */
  bool autoTip = true;
  size_t blockGets = 0;

  std::optional<Client::SignedTip> statusTip() const {
    if (signedTip) {
      return signedTip;
    }
    return autoTip && !chain.empty() ? std::optional<Client::SignedTip>(tipFor(chain.back())) : std::nullopt;
  }
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
    if (request.type == Client::T_REQ_BLOCK_WAIT) {
      Client::TipReply reply;
      reply.nextBlockId = chain.size();
      reply.setTip(signedTip);
      response.payload = utl::binaryPack(reply);
    } else if (request.type == Client::T_REQ_STATUS) {
      response.payload = statusPayload(chain, statusTip());
    } else {
      ++blockGets;
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

class BlockSyncTest : public ::testing::Test {
protected:
  void SetUp() override {
    upstream_->chain = makeChain(10);
    local_.assign(upstream_->chain.begin(), upstream_->chain.begin() + 3);
    client_.setTransport(std::unique_ptr<ILedgerTransport>(upstream_));
  }

  void runToEnd() {
    while (!upstream_->pending.empty() || !tasks_.empty()) {
      upstream_->completeAll();
      drain();
    }
  }

  BlockSync::Hooks hooks() {
    BlockSync::Hooks h;
    h.dialUpstream = []() { return pp::Roe<void>(); };
    h.nextBlockId = [this]() { return static_cast<uint64_t>(local_.size()); };
    h.tipHash = [this]() {
      return local_.empty() ? std::optional<std::string>() : std::optional<std::string>(local_.back().hash);
    };
    h.authorityTrail = testTrail;
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
  upstream_->complete(0);  // status
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
  EXPECT_EQ(local_.size(), 3u);  // nothing commits before the signed tip is reached
  runToEnd();
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
  local_.push_back(upstream_->chain[3]);
  local_.push_back(upstream_->chain[4]);
  while (!upstream_->pending.empty() || !tasks_.empty()) {
    upstream_->completeAll();
    drain();
  }
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 10u);
}

// --- beacon-authority ba4: commit only what a verified signed tip vouches for ---

TEST_F(BlockSyncTest, ATipThatDoesNotVerifyFetchesNothing) {
  upstream_->signedTip = tipFor(upstream_->chain.back(), utl::mlDsaGenerate().value());
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("does not verify"), std::string::npos) << results_[0].error;
  EXPECT_EQ(upstream_->blockGets, 0u);
  EXPECT_EQ(local_.size(), 3u);
}

TEST_F(BlockSyncTest, AnUpstreamWithoutASignedTipIsNotFollowed) {
  upstream_->autoTip = false;
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_EQ(local_.size(), 3u);
}

// A genuine tip, but the blocks served under it are not the chain it signed: nothing commits.
TEST_F(BlockSyncTest, BlocksThatDoNotLeadToTheSignedTipCommitNothing) {
  auto other = upstream_->chain.back();
  other.block.timestamp = 42; // a different block 9
  upstream_->signedTip = tipFor(Ledger::ChainNode{other.block, chain_block::calculateBlockHash(other.block)});
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("do not lead to the signed tip"), std::string::npos) << results_[0].error;
  EXPECT_EQ(local_.size(), 3u);
}

TEST_F(BlockSyncTest, ABlockThatDoesNotLinkCommitsNothing) {
  upstream_->chain[5] = makeBlock(5, "elsewhere");
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("does not extend"), std::string::npos) << results_[0].error;
  EXPECT_EQ(local_.size(), 3u);
}

TEST_F(BlockSyncTest, CommitsOnlyUpToTheSignedTip) {
  upstream_->signedTip = tipFor(upstream_->chain[6]); // blocks 7..9 exist but are not signed yet
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 7u);
}

TEST_F(BlockSyncTest, ALocalTipTheBeaconDidNotSignIsReported) {
  local_.back() = makeBlock(2, local_[1].hash);
  local_.back().block.timestamp = 7;
  local_.back().hash = chain_block::calculateBlockHash(local_.back().block);
  upstream_->chain.resize(3); // signed tip = upstream's block 2
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("not the one the beacon signed"), std::string::npos) << results_[0].error;
}

// A long catch-up keeps at most maxHeldBlocks bodies; the rest are fetched again to apply.
TEST_F(BlockSyncTest, BeyondTheHeldBlocksBodiesAreFetchedAgain) {
  BlockSync::Options options;
  options.maxHeldBlocks = 2;
  BlockSync sync(client_, hooks(), options);
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 10u);
  EXPECT_EQ(upstream_->blockGets, 7u + 5u);
}

TEST_F(BlockSyncTest, ANodeWithNoBlocksTakesTheFirstOnTrust) {
  local_.clear();
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 10u);
  EXPECT_EQ(results_[0].blocksAdded, 10u);
}

// --- beacon-authority ba5: follow the generation the admin set certifies along the range ---

TEST_F(BlockSyncTest, FollowsAGenerationCertifiedInTheRange) {
  const auto nextKey = utl::mlDsaGenerate().value();
  upstream_->chain = makeChain(10, {{5, {certRecord(2, nextKey, 4)}}});
  local_.assign(upstream_->chain.begin(), upstream_->chain.begin() + 3);
  upstream_->signedTip = tipFor(upstream_->chain.back(), nextKey, 2);
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 10u);
}

TEST_F(BlockSyncTest, TheOldGenerationAboveTheCertificateIsSuperseded) {
  upstream_->chain = makeChain(10, {{5, {certRecord(2, utl::mlDsaGenerate().value(), 4)}}});
  local_.assign(upstream_->chain.begin(), upstream_->chain.begin() + 3);
  upstream_->signedTip = tipFor(upstream_->chain.back()); // generation 1 still signing at block 9
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("superseded"), std::string::npos) << results_[0].error;
  EXPECT_EQ(local_.size(), 3u);
}

TEST_F(BlockSyncTest, ACertificateTheAdminSetDidNotSignMovesNothing) {
  const auto thiefKey = utl::mlDsaGenerate().value();
  upstream_->chain = makeChain(10, {{5, {certRecord(2, thiefKey, 4, /*signer=*/thiefKey)}}});
  local_.assign(upstream_->chain.begin(), upstream_->chain.begin() + 3);
  upstream_->signedTip = tipFor(upstream_->chain.back(), thiefKey, 2);
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("not signed by the admin set"), std::string::npos) << results_[0].error;
  EXPECT_EQ(local_.size(), 3u);
}

TEST_F(BlockSyncTest, ALaterGenerationNoBlockCertifiesIsRefused) {
  upstream_->signedTip = tipFor(upstream_->chain.back(), utl::mlDsaGenerate().value(), 2);
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_EQ(local_.size(), 3u);
}

TEST_F(BlockSyncTest, APinnedFirstBlockMustMatch) {
  local_.clear();
  std::optional<std::string> pinned = "another genesis";
  auto h = hooks();
  h.firstBlockHash = [&]() { return pinned; };
  BlockSync refused(client_, h);
  refused.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_TRUE(local_.empty());

  pinned = upstream_->chain[0].hash;
  BlockSync taken(client_, h);
  taken.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 2u);
  EXPECT_TRUE(results_[1].ok) << results_[1].error;
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
    if (request.type == Client::T_REQ_STATUS) {
      response.payload = statusPayload(chain, tipFor(chain.back()));
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
    upstream_->chain = makeChain(6);
    client_.setTransport(std::unique_ptr<ILedgerTransport>(upstream_));
    BlockSync::Hooks h;
    h.dialUpstream = []() { return pp::Roe<void>(); };
    h.nextBlockId = [this]() { return static_cast<uint64_t>(local_.size()); };
    h.tipHash = [this]() {
      return local_.empty() ? std::optional<std::string>() : std::optional<std::string>(local_.back().hash);
    };
    h.authorityTrail = testTrail;
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

// --- UpstreamTipWatch: BLOCK_WAIT keeps a downstream close behind its upstream ---

namespace {

class TipWatchTest : public BlockSyncTest {
protected:
  UpstreamTipWatch::Hooks watchHooks() {
    UpstreamTipWatch::Hooks h;
    h.dialUpstream = []() { return pp::Roe<void>(); };
    h.nextBlockId = [this]() { return static_cast<uint64_t>(local_.size()); };
    h.onUpstreamAhead = [this]() { ++syncRequests_; };
    h.onSignedTip = [this](const Client::SignedTip &tip) { tips_.push_back(tip); };
    h.postToServerThread = [this](std::function<void()> task) { tasks_.push_back(std::move(task)); };
    return h;
  }
  uint64_t knownOf(size_t index) {
    return utl::binaryUnpack<uint64_t>(upstream_->pending.at(index).first.payload).value();
  }
  int syncRequests_ = 0;
  std::vector<Client::SignedTip> tips_;
};

TEST_F(TipWatchTest, KeepsOneWaitOutAndRequestsSyncWhenUpstreamIsAhead) {
  UpstreamTipWatch watch(client_, watchHooks());
  watch.maintain();
  watch.maintain();
  ASSERT_EQ(upstream_->pending.size(), 1u); // one wait at a time
  EXPECT_EQ(upstream_->pending[0].first.type, Client::T_REQ_BLOCK_WAIT);
  EXPECT_EQ(knownOf(0), 3u);

  upstream_->complete(0); // upstream has 10 blocks
  drain();
  EXPECT_EQ(syncRequests_, 1);

  // While that sync runs the local tip is still 3: wait for blocks beyond the
  // upstream's 10, not 3 (which would be answered at once, i.e. polling).
  watch.maintain();
  ASSERT_EQ(upstream_->pending.size(), 1u);
  EXPECT_EQ(knownOf(0), 10u);
}

TEST_F(TipWatchTest, NoSyncWhenUpstreamIsNotAhead) {
  upstream_->chain.resize(3);
  UpstreamTipWatch watch(client_, watchHooks());
  watch.maintain();
  upstream_->complete(0); // hold ended, tip unchanged
  drain();
  EXPECT_EQ(syncRequests_, 0);
  watch.maintain();
  EXPECT_EQ(upstream_->pending.size(), 1u); // re-armed
}

// beacon-authority ba3: the signed tip rides the BLOCK_WAIT reply and reaches the node's hook.
TEST_F(TipWatchTest, SignedTipInTheReplyIsHandedOn) {
  Client::SignedTip tip;
  tip.networkId = "net";
  tip.generation = 1;
  tip.height = 9;
  tip.blockHash = "hash-9";
  tip.signature = "sig";
  upstream_->signedTip = tip;
  UpstreamTipWatch watch(client_, watchHooks());
  watch.maintain();
  upstream_->complete(0);
  drain();
  ASSERT_EQ(tips_.size(), 1u);
  EXPECT_EQ(tips_[0], tip);

  upstream_->signedTip.reset(); // an upstream with no signed tip yet
  watch.maintain();
  upstream_->complete(0);
  drain();
  EXPECT_EQ(tips_.size(), 1u);
}

TEST_F(TipWatchTest, FailureBacksOffInsteadOfRetryingAtOnce) {
  UpstreamTipWatch watch(client_, watchHooks());
  watch.maintain();
  upstream_->complete(0, /*fail=*/true); // e.g. an upstream that does not serve BLOCK_WAIT
  drain();
  watch.maintain();
  EXPECT_TRUE(upstream_->pending.empty());
  EXPECT_FALSE(watch.inFlight());
  EXPECT_EQ(syncRequests_, 0);
}

} // namespace
