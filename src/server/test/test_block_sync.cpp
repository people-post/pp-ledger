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

/** The generation-1 beacon key of the test chain. */
const utl::MlDsaKeyPair &beaconKey() {
  static const utl::MlDsaKeyPair key = utl::mlDsaGenerate().value();
  return key;
}

/** The test chain's admin set: one key. */
const utl::MlDsaKeyPair &adminKey() {
  static const utl::MlDsaKeyPair key = utl::mlDsaGenerate().value();
  return key;
}

/** The test chain's genesis meta: network "net", generation 1 = beaconKey(), admin set = adminKey(). */
GenesisAccountMeta testGenesisMeta() {
  GenesisAccountMeta gm;
  gm.config.networkId = "net";
  gm.authority.beacon.networkId = "net";
  gm.authority.beacon.generation = 1;
  gm.authority.beacon.publicKey = beaconKey().publicKey;
  gm.genesis.wallet.publicKeys = {adminKey().publicKey};
  gm.genesis.wallet.minSignatures = 1;
  gm.genesis.wallet.keyType = Crypto::TK_ML_DSA_65;
  return gm;
}

/** Genesis's record: where a node starting at block 0 learns the authority. */
Ledger::Record genesisRecord() {
  Ledger::TxGenesis tx;
  tx.meta = testGenesisMeta().ltsToString();
  Ledger::Record record;
  record.type = Ledger::T_GENESIS;
  record.data = utl::binaryPack(tx);
  return record;
}

/** A genesis renewal carrying `gm`: what a checkpoint block opens with. */
Ledger::Record genesisRenewal(const GenesisAccountMeta &gm = testGenesisMeta()) {
  Ledger::TxRenewal tx;
  tx.walletId = AccountBuffer::ID_GENESIS;
  tx.meta = gm.ltsToString();
  Ledger::Record record;
  record.type = Ledger::T_RENEWAL;
  record.data = utl::binaryPack(tx);
  return record;
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
    } else if (i == 0) {
      node.block.records = {genesisRecord()};
    }
    node.hash = chain_block::calculateBlockHash(node.block);
    chain.push_back(node);
  }
  return chain;
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

/** The authority at the local tip of a test node holding blocks: what genesis set. */
pp::Roe<AuthorityTrail> testTrail() {
  const auto gm = testGenesisMeta();
  return AuthorityTrail("net", gm.authority, gm.genesis.wallet);
}

/** A T_BEACON_CERT for `generation` with `key`, signed by `signer` (the admin set's key unless given). */
Ledger::Record certRecord(uint64_t generation, const utl::MlDsaKeyPair &key, uint64_t cutoverHeight,
                          const utl::MlDsaKeyPair &signer = adminKey(),
                          const std::string &cutoverHash = std::string(32, 'c')) {
  BeaconCertificate cert;
  cert.networkId = "net";
  cert.generation = generation;
  cert.publicKey = key.publicKey;
  cert.cutoverHeight = cutoverHeight;
  cert.cutoverHash = cert.hasCutover() ? cutoverHash : std::string();
  Ledger::TxBeaconCert tx;
  tx.meta = cert.ltsToString();
  Ledger::Record record;
  record.type = Ledger::T_BEACON_CERT;
  record.data = utl::binaryPack(tx);
  record.signatures = {utl::mlDsaSign(signer.privateKey, record.signingMessage("net")).value()};
  return record;
}

/** STATUS as an upstream serves it: its next block id, signed tip and current certificate. */
std::string statusPayload(const std::vector<Ledger::ChainNode> &chain, const std::optional<Client::SignedTip> &tip,
                          const std::optional<Ledger::Record> &certificate = std::nullopt,
                          const std::optional<Client::Equivocation> &equivocation = std::nullopt) {
  Client::BeaconState state;
  state.nextBlockId = chain.size();
  state.signedTip = tip;
  state.certificate = certificate;
  state.equivocation = equivocation;
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
  /** The certificate STATUS serves (beacon-authority ba5c). */
  std::optional<Ledger::Record> certificate;
  /** The equivocation evidence STATUS serves (beacon-authority ba6). */
  std::optional<Client::Equivocation> equivocation;
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
      response.payload = statusPayload(chain, statusTip(), certificate, equivocation);
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
    h.nextBlockId = [this]() { return startAt_ + static_cast<uint64_t>(local_.size()); };
    h.tipHash = [this]() {
      return local_.empty() ? std::optional<std::string>() : std::optional<std::string>(local_.back().hash);
    };
    h.authorityTrail = testTrail;
    h.applyBlock = [this](const Ledger::ChainNode &block) -> pp::Roe<void> {
      if (block.block.index != startAt_ + local_.size()) {
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
  /** The block a node with no blocks starts at (its ledger's starting block id). */
  uint64_t startAt_ = 0;
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

// --- beacon-authority ba5b.5: a node with no blocks authenticates its range from its first block ---

TEST_F(BlockSyncTest, ANodeWithNoBlocksStartsFromGenesis) {
  local_.clear();
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 10u);
  EXPECT_EQ(results_[0].blocksAdded, 10u);
}

// The first block is committed only with the rest, once the tip verifies under the authority it
// carries: a forged tip leaves the node with nothing.
TEST_F(BlockSyncTest, ANodeWithNoBlocksCommitsNothingBeforeTheTipVerifies) {
  local_.clear();
  upstream_->signedTip = tipFor(upstream_->chain.back(), utl::mlDsaGenerate().value());
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("does not verify"), std::string::npos) << results_[0].error;
  EXPECT_TRUE(local_.empty());
  EXPECT_GT(upstream_->blockGets, 0u) << "it authenticated the range first";
}

TEST_F(BlockSyncTest, ANodeWithNoBlocksStartsAtACheckpoint) {
  upstream_->chain = makeChain(10, {{4, {genesisRenewal()}}});
  local_.clear();
  startAt_ = 4;
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 6u);
  EXPECT_EQ(local_.front().block.index, 4u);
}

TEST_F(BlockSyncTest, AStartThatIsNotACheckpointIsRefused) {
  local_.clear();
  startAt_ = 4; // block 4 carries no genesis renewal
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("not a checkpoint"), std::string::npos) << results_[0].error;
  EXPECT_TRUE(local_.empty());
}

// A later genesis renewal re-records the authority in force; one that disagrees (a key no
// certificate named) fails the sync.
TEST_F(BlockSyncTest, AGenesisRenewalThatDisagreesFailsTheSync) {
  auto forged = testGenesisMeta();
  forged.authority.beacon.publicKey = utl::mlDsaGenerate().value().publicKey;
  upstream_->chain = makeChain(10, {{6, {genesisRenewal(forged)}}});
  local_.assign(upstream_->chain.begin(), upstream_->chain.begin() + 3);
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("disagrees"), std::string::npos) << results_[0].error;
  EXPECT_EQ(local_.size(), 3u);
}

TEST_F(BlockSyncTest, TheStartingAuthorityCanBeVetted) {
  upstream_->chain = makeChain(10, {{4, {genesisRenewal()}}});
  local_.clear();
  startAt_ = 4;
  auto h = hooks();
  std::optional<uint64_t> vettedAt;
  h.checkStartingAuthority = [&](uint64_t blockId, const ChainAuthority &authority, const Client::Wallet &admin) {
    vettedAt = blockId;
    EXPECT_EQ(authority.beacon.publicKey, beaconKey().publicKey);
    EXPECT_EQ(admin.publicKeys, std::vector<std::string>{adminKey().publicKey});
    return pp::Roe<void>(pp::Error("not the anchored authority"));
  };
  BlockSync sync(client_, h);
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_EQ(vettedAt, std::optional<uint64_t>(4));
  EXPECT_TRUE(local_.empty());
}

// --- beacon-authority ba5: follow the generation the admin set certifies along the range ---

TEST_F(BlockSyncTest, FollowsAGenerationCertifiedInTheRange) {
  const auto nextKey = utl::mlDsaGenerate().value();
  upstream_->chain = makeChain(10, {{5, {certRecord(2, nextKey, BeaconCertificate::kNoCutover)}}});
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
  upstream_->chain = makeChain(10, {{5, {certRecord(2, utl::mlDsaGenerate().value(), BeaconCertificate::kNoCutover)}}});
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
  upstream_->chain = makeChain(10, {{5, {certRecord(2, thiefKey, BeaconCertificate::kNoCutover, /*signer=*/thiefKey)}}});
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

// --- beacon-authority ba5c: a later generation seen out of band bounds the older one's tips ---

// The admins certified generation 2 with cutover 6 (generation 1's key was stolen); the upstream
// serves the certificate on STATUS but a tip of generation 1 at block 9. The node takes the
// certificate before it checks the tip, so it refuses the tip and commits nothing.
TEST_F(BlockSyncTest, ATipPastTheCutoverOfACertificateOnStatusIsRefused) {
  upstream_->certificate = certRecord(2, utl::mlDsaGenerate().value(), 6);
  std::optional<Ledger::Record> seen;
  auto h = hooks();
  h.offerCertificate = [&](const Ledger::Record &record) { seen = record; };
  h.authorityTrail = [&]() -> pp::Roe<AuthorityTrail> {
    auto trail = testTrail();
    if (seen) {
      BeaconCertificate cert;
      EXPECT_TRUE(cert.ltsFromString(utl::binaryUnpack<Ledger::TxBeaconCert>(seen->data).value().meta));
      trail.value().setSeenCertificate(cert);
    }
    return trail;
  };
  BlockSync sync(client_, h);
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("past block 6"), std::string::npos) << results_[0].error;
  EXPECT_EQ(upstream_->blockGets, 0u);
  EXPECT_EQ(local_.size(), 3u);

  // Up to the cutover, generation 1 still signs.
  upstream_->signedTip = tipFor(upstream_->chain[6]);
  BlockSync upToCutover(client_, h);
  upToCutover.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 2u);
  EXPECT_TRUE(results_[1].ok) << results_[1].error;
  EXPECT_EQ(local_.size(), 7u);
}

// beacon-authority ba6: evidence on STATUS is offered before the tip is checked; a node holding it
// syncs nothing of that generation above its height.
TEST_F(BlockSyncTest, ATipPastEquivocationEvidenceOnStatusIsRefused) {
  Client::SignedTip fork = tipFor(upstream_->chain[5]);
  fork.blockHash = "another block 5";
  fork.signature = utl::mlDsaSign(beaconKey().privateKey, fork.signingMessage()).value();
  upstream_->equivocation = Client::Equivocation::of(tipFor(upstream_->chain[5]), fork);
  ASSERT_TRUE(upstream_->equivocation.has_value());
  std::optional<Client::Equivocation> held;
  auto h = hooks();
  h.offerEquivocation = [&](const Client::Equivocation &evidence) { held = evidence; };
  h.authorityTrail = [&]() -> pp::Roe<AuthorityTrail> {
    auto trail = testTrail();
    trail.value().setEquivocation(held);
    return trail;
  };
  BlockSync sync(client_, h);
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("equivocation"), std::string::npos) << results_[0].error;
  EXPECT_EQ(local_.size(), 3u);

  // Up to its height the generation still signs.
  upstream_->signedTip = tipFor(upstream_->chain[5]);
  BlockSync upToHeight(client_, h);
  upToHeight.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 2u);
  EXPECT_TRUE(results_[1].ok) << results_[1].error;
  EXPECT_EQ(local_.size(), 6u);
}

// --- beacon-authority bp2: a certificate's cutover block, vouched for by its hash ---

namespace {

/** `chain` grown to `length` blocks, `records` in some of the new ones. */
void extendChain(std::vector<Ledger::ChainNode> &chain, size_t length,
                 const std::map<uint64_t, std::vector<Ledger::Record>> &records = {}) {
  while (chain.size() < length) {
    Ledger::ChainNode node;
    node.block.index = chain.size();
    node.block.previousHash = chain.back().hash;
    if (auto it = records.find(node.block.index); it != records.end()) {
      node.block.records = it->second;
    }
    node.hash = chain_block::calculateBlockHash(node.block);
    chain.push_back(node);
  }
}

} // namespace

// Once the old generation is refused past a cutover, no tip vouches for the blocks up to it: a node
// behind it syncs to the block its held certificate vouches for, by hash, with no signed tip.
TEST_F(BlockSyncTest, ANodeBehindItsCertificatesCutoverSyncsToItWithNoTip) {
  upstream_->autoTip = false; // nothing signed past the node's tip
  std::optional<std::pair<uint64_t, std::string>> vouched = std::make_pair(uint64_t{6}, upstream_->chain[6].hash);
  auto h = hooks();
  h.vouchedCutover = [&]() { return vouched; };
  BlockSync sync(client_, h);
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 7u) << "up to the vouched block, not past it";

  local_.resize(3);
  vouched->second = upstream_->chain[5].hash; // not block 6
  BlockSync wrong(client_, h);
  wrong.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 2u);
  EXPECT_FALSE(results_[1].ok);
  EXPECT_NE(results_[1].error.find("vouches for"), std::string::npos) << results_[1].error;
  EXPECT_EQ(local_.size(), 3u) << "nothing commits";
}

// A node with no blocks takes the upstream's STATUS certificate: checked against the authority it
// builds from its first block, it bounds the old generation's tip and the range ends at its cutover.
TEST_F(BlockSyncTest, ANodeWithNoBlocksStopsAtTheCutoverItsUpstreamServes) {
  local_.clear();
  upstream_->certificate = certRecord(2, utl::mlDsaGenerate().value(), 6, adminKey(), upstream_->chain[6].hash);
  BlockSync sync(client_, hooks()); // the old generation still signs block 9
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_TRUE(results_[0].ok) << results_[0].error;
  EXPECT_EQ(local_.size(), 7u) << "blocks 0 to 6: the old generation's tip past the cutover is not followed";

  local_.clear();
  const auto thief = utl::mlDsaGenerate().value();
  upstream_->certificate = certRecord(2, utl::mlDsaGenerate().value(), 6, thief, upstream_->chain[6].hash);
  BlockSync forged(client_, hooks());
  forged.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 2u);
  EXPECT_FALSE(results_[1].ok) << "a certificate the admin set did not sign vouches for nothing";
  EXPECT_TRUE(local_.empty());
}

// On chain, a certificate with a cutover commits right after the block it vouches for, by its hash.
TEST_F(BlockSyncTest, ACertificateWhoseHashIsNotItsCutoverBlockStopsTheSync) {
  const auto nextKey = utl::mlDsaGenerate().value();
  auto chain = makeChain(5);
  extendChain(chain, 10, {{5, {certRecord(2, nextKey, 4, adminKey(), std::string(32, 'x'))}}});
  upstream_->chain = chain;
  local_.assign(chain.begin(), chain.begin() + 3);
  upstream_->signedTip = tipFor(chain.back(), nextKey, 2);
  BlockSync sync(client_, hooks());
  sync.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("is not the hash of block 4"), std::string::npos) << results_[0].error;
  EXPECT_EQ(local_.size(), 3u);

  chain.resize(5);
  extendChain(chain, 10, {{5, {certRecord(2, nextKey, 4, adminKey(), chain[4].hash)}}});
  upstream_->chain = chain;
  upstream_->signedTip = tipFor(chain.back(), nextKey, 2);
  BlockSync good(client_, hooks());
  good.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 2u);
  EXPECT_TRUE(results_[1].ok) << results_[1].error;
  EXPECT_EQ(local_.size(), 10u);
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

// beacon-authority B037: a node syncing from block 0 runs its genesis check (a re-genesis's
// predecessor against its old ledger); refused, nothing is taken.
TEST_F(BlockSyncTest, AGenesisTheNodeRefusesIsNotTaken) {
  local_.clear();
  bool accept = false;
  auto h = hooks();
  h.checkGenesis = [&](const Ledger::ChainNode &genesis) -> pp::Roe<void> {
    EXPECT_EQ(genesis.block.index, 0u);
    return accept ? pp::Roe<void>() : pp::Roe<void>(pp::Error("not the predecessor"));
  };
  BlockSync refused(client_, h);
  refused.start();
  runToEnd();
  ASSERT_EQ(results_.size(), 1u);
  EXPECT_FALSE(results_[0].ok);
  EXPECT_NE(results_[0].error.find("not the predecessor"), std::string::npos) << results_[0].error;
  EXPECT_TRUE(local_.empty());

  accept = true;
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
  EXPECT_EQ(server_.runStartupSync(*sync_, last_, std::chrono::seconds(5)), "");
  EXPECT_EQ(local_.size(), 6u);
}

// A stop request ends the startup sync at once (the node then does not run).
TEST_F(StartupSyncTest, AStopRequestEndsIt) {
  upstream_->failures = 1000000;
  server_.setStop(true);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(server_.runStartupSync(*sync_, last_, std::chrono::seconds(30)), "");
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
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
