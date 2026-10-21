// The verifying gateway (projects/finality fn2) against chains in this process: what it follows, what
// it refuses, what it keeps across a restart, and how it meets a takeover's rewind and an equivocation.

#include "gateway/Gateway.h"

#include "chain/test/ChainFixtures.h"
#include "testing/TestTempPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

namespace pp::gateway {
namespace {

using namespace pp::chain_test;

/** A node serving `chain`, its beacon signing every tip; what its STATUS adds is the test's to set. */
struct ChainSource : Source {
  Chain *chain{nullptr};
  utl::MlDsaKeyPair beaconKey = testBeaconKey();
  uint64_t generation{1};
  std::optional<Ledger::Record> certificate;
  std::optional<Client::Equivocation> equivocation;
  /** Alters the block at this height as served (a node lying). */
  std::optional<uint64_t> alter;

  explicit ChainSource(Chain &c) : chain(&c) {}

  static Client::Error refused(const std::string &message) { return Client::Error(-1, message); }

  Client::SignedTip signTip(uint64_t height) const {
    Client::SignedTip tip;
    tip.networkId = chain->getNetworkId();
    tip.generation = generation;
    tip.height = height;
    tip.blockHash = chain->readBlock(height).value().hash;
    tip.signature = utl::mlDsaSign(beaconKey.privateKey, tip.signingMessage()).value();
    return tip;
  }

  /** The bound in force the node reports (STATUS finalitySeconds); none: the chain's. */
  std::optional<uint64_t> reportedFinality;

  Roe<Client::BeaconState> status() override {
    Client::BeaconState state;
    state.networkId = chain->getNetworkId();
    state.finalitySeconds = reportedFinality.value_or(chain->getFinalitySeconds());
    state.nextBlockId = chain->getNextBlockId();
    state.signedTip = signTip(chain->getNextBlockId() - 1);
    state.certificate = certificate;
    state.equivocation = equivocation;
    return state;
  }
  Roe<Ledger::ChainNode> block(uint64_t height) override {
    auto found = chain->readBlock(height);
    if (!found) {
      return refused(found.error().message);
    }
    if (alter && *alter == height) {
      found.value().block.timestamp += 1;
    }
    return found.value();
  }
  Roe<Client::AuthorityProof> authorityProof(uint64_t upToBlockId) override {
    auto proof = chain->authorityProof(upToBlockId);
    return proof ? Roe<Client::AuthorityProof>(proof.value()) : Roe<Client::AuthorityProof>(refused(proof.error().message));
  }
  Roe<Client::AccountProof> accountProof(uint64_t accountId) override {
    auto proof = chain->accountProof(accountId);
    return proof ? Roe<Client::AccountProof>(proof.value()) : Roe<Client::AccountProof>(refused(proof.error().message));
  }
};

class GatewayTest : public ::testing::Test {
protected:
  void SetUp() override {
    harness_.SetUp();
    dataDir_ = pp::test::uniqueTempPath("pp-ledger-gateway");
    std::filesystem::remove_all(dataDir_);
    tip_ = harness_.genesis;
  }
  void TearDown() override {
    gateway_.reset();
    std::filesystem::remove_all(dataDir_);
    harness_.TearDown();
  }

  Gateway &open(Source &source, std::string genesisHash = {}) {
    gateway_.reset();
    Gateway::Options options;
    options.genesisHash = genesisHash.empty() ? harness_.genesis.hash : genesisHash;
    options.dataDir = dataDir_;
    gateway_ = std::make_unique<Gateway>(source, options);
    auto opened = gateway_->open();
    EXPECT_TRUE(opened.isOk()) << opened.error().message;
    return *gateway_;
  }

  void poll() {
    auto polled = gateway_->poll();
    ASSERT_TRUE(polled.isOk()) << polled.error().message;
  }

  /** The next block on the producer at `slot`, committed. */
  const Ledger::ChainNode &add(uint64_t slot, std::vector<Ledger::Record> records) {
    tip_ = makeNextBlockAtSlot(harness_.producer, tip_, slot, records);
    EXPECT_TRUE(harness_.producer.addBlock(tip_).isOk());
    return tip_;
  }

  Ledger::Record newUser(uint64_t id, int64_t balance) {
    Ledger::TxNewUser tx;
    tx.fromWalletId = AccountBuffer::ID_RESERVE;
    tx.toWalletId = id;
    tx.amount = static_cast<uint64_t>(balance);
    tx.fee = 6; // meta fee 1 + newAccountFee 5
    tx.meta = makeUserAccount(makeKeyPair().publicKey, balance).ltsToString();
    tx.idempotentId = idem_++;
    tx.validationTsMin = harness_.chainConfig.genesisTime;
    tx.validationTsMax = harness_.chainConfig.genesisTime + 3600;
    return makeRecord(Ledger::T_NEW_USER, tx, harness_.reserveKey, harness_.producer.getNetworkId());
  }

  Ledger::Record transfer(uint64_t to, uint64_t amount) {
    Ledger::TxDefault tx;
    tx.tokenId = AccountBuffer::ID_GENESIS;
    tx.fromWalletId = AccountBuffer::ID_RESERVE;
    tx.toWalletId = to;
    tx.amount = amount;
    tx.fee = 1;
    tx.idempotentId = idem_++;
    tx.validationTsMin = harness_.chainConfig.genesisTime;
    tx.validationTsMax = harness_.chainConfig.genesisTime + 3600;
    return makeRecord(Ledger::T_DEFAULT, tx, harness_.reserveKey, harness_.producer.getNetworkId());
  }

  int64_t balanceAt(uint64_t id, At at) {
    auto balances = gateway_->balances(id, at);
    EXPECT_TRUE(balances.isOk()) << balances.error().message;
    if (!balances || !balances.value().present) {
      return -1;
    }
    return balances.value().balances[AccountBuffer::ID_GENESIS];
  }

  int64_t slotTime(uint64_t slot) const {
    return harness_.chainConfig.genesisTime + static_cast<int64_t>(slot * harness_.chainConfig.slotDuration);
  }

  ComposeHarness harness_;
  std::filesystem::path dataDir_;
  std::unique_ptr<Gateway> gateway_;
  Ledger::ChainNode tip_;
  uint64_t idem_{1};
};

constexpr uint64_t kUser = AccountBuffer::ID_FIRST_USER + 1;

At height(uint64_t h) { return At{At::Height, static_cast<int64_t>(h)}; }

TEST_F(GatewayTest, FollowsVerifiedBlocksAndProvesTheAccountsItWatches) {
  ChainSource source(harness_.producer);
  Gateway &gateway = open(source);
  gateway.setClockOverride(slotTime(0));
  EXPECT_EQ(gateway.tip().error().code, Gateway::kUnavailable) << "nothing followed yet";
  poll();
  EXPECT_EQ(gateway.tip().value().block.height, 0u);
  ASSERT_TRUE(gateway.watch(kUser).isOk());
  EXPECT_EQ(gateway.balances(kUser, At{}).error().code, Gateway::kConflict) << "not proven yet";
  poll();
  auto none = gateway.balances(kUser, At{});
  ASSERT_TRUE(none.isOk()) << none.error().message;
  EXPECT_FALSE(none.value().present) << "no such account yet";

  add(1, {newUser(kUser, 1000)});
  poll();
  add(2, {transfer(kUser, 10)});
  gateway.setClockOverride(slotTime(2));
  poll();
  auto tip = gateway.tip().value();
  EXPECT_EQ(tip.block.height, 2u);
  EXPECT_EQ(tip.block.hash, tip_.hash);
  EXPECT_EQ(tip.generation, 1u);
  EXPECT_EQ(tip.finalitySeconds, 3600u);
  EXPECT_EQ(tip.finalizedHeight, std::nullopt);
  EXPECT_TRUE(tip.halted.empty());
  EXPECT_EQ(balanceAt(kUser, At{}), 1010);
  EXPECT_EQ(balanceAt(kUser, height(1)), 1000) << "by height";
  EXPECT_EQ(balanceAt(kUser, height(0)), -1);
  EXPECT_EQ(balanceAt(kUser, At{At::Time, slotTime(1) + 4}), 1000) << "the last block whose slot started by then";
  EXPECT_EQ(gateway.balances(kUser, height(3)).error().code, Gateway::kConflict) << "not verified yet";
  EXPECT_EQ(gateway.balances(kUser + 1, At{}).error().code, Gateway::kNotFound) << "not watched";

  // Final by the gateway's clock: each block an hour after its slot's start.
  EXPECT_EQ(gateway.balances(kUser, At{At::Final}).error().code, Gateway::kConflict);
  gateway.setClockOverride(slotTime(1) + 3600);
  poll();
  EXPECT_EQ(gateway.tip().value().finalizedHeight, std::optional<uint64_t>(1));
  EXPECT_EQ(balanceAt(kUser, At{At::Final}), 1000);
  EXPECT_TRUE(gateway.blockAt(1).value().final);
  EXPECT_FALSE(gateway.blockAt(2).value().final);
  EXPECT_EQ(gateway.blockAt(2).value().finalityTime, slotTime(2) + 3600);
  gateway.setClockOverride(slotTime(2) + 3600);
  poll();
  EXPECT_EQ(balanceAt(kUser, At{At::Final}), 1010);

  // Changed twice between two proofs: known after the last, not between.
  add(3, {transfer(kUser, 10)});
  add(4, {transfer(kUser, 10)});
  poll();
  EXPECT_EQ(balanceAt(kUser, At{}), 1030);
  EXPECT_EQ(gateway.balances(kUser, height(3)).error().code, Gateway::kConflict);
  EXPECT_EQ(balanceAt(kUser, height(2)), 1010);

  ASSERT_TRUE(gateway.unwatch(kUser).isOk());
  EXPECT_EQ(gateway.balances(kUser, At{}).error().code, Gateway::kNotFound);
}

// A node answering for the watched account with a genuine proof of something else: another
// account, an unused id, or the account at a block older than the one verified here.
TEST_F(GatewayTest, TakesOnlyAProofOfTheAccountAskedAtTheVerifiedTip) {
  struct AnsweringSource : ChainSource {
    using ChainSource::ChainSource;
    std::optional<Client::AccountProof> answer;
    Roe<Client::AccountProof> accountProof(uint64_t accountId) override {
      if (answer) {
        return *answer;
      }
      return ChainSource::accountProof(accountId);
    }
  };
  AnsweringSource source(harness_.producer);
  Gateway &gateway = open(source);
  gateway.setClockOverride(slotTime(0));
  add(1, {newUser(kUser, 1000), newUser(kUser + 1, 5000)});
  poll();
  const Client::AccountProof before = harness_.producer.accountProof(kUser).value();
  add(2, {transfer(kUser, 10)});
  gateway.setClockOverride(slotTime(2));
  poll();
  ASSERT_TRUE(gateway.watch(kUser).isOk());

  source.answer = harness_.producer.accountProof(kUser + 1).value();
  poll();
  EXPECT_EQ(gateway.balances(kUser, At{}).error().code, Gateway::kConflict) << "another account's proof";
  source.answer = harness_.producer.accountProof(kUser + 50).value();
  poll();
  EXPECT_EQ(gateway.balances(kUser, At{}).error().code, Gateway::kConflict) << "an unused id's absence";
  source.answer = before;
  poll();
  EXPECT_EQ(gateway.balances(kUser, At{}).error().code, Gateway::kConflict) << "a proof from before block 2";

  source.answer.reset();
  poll();
  EXPECT_EQ(balanceAt(kUser, At{}), 1010) << "the node's own proof at its tip";
  source.answer = before;
  add(3, {transfer(kUser + 1, 10)}); // not naming the watched account
  poll();
  EXPECT_EQ(balanceAt(kUser, At{}), 1010) << "an old proof in a later round changes nothing";
}

TEST_F(GatewayTest, RefusesWhatItCannotVerify) {
  ChainSource source(harness_.producer);
  open(source, std::string(32, '\x01'));
  EXPECT_FALSE(gateway_->poll().isOk()) << "not the pinned genesis";

  open(source);
  poll();
  add(1, {transfer(AccountBuffer::ID_REGISTRAR, 10)});
  source.beaconKey = makeKeyPair();
  EXPECT_FALSE(gateway_->poll().isOk()) << "a tip not the beacon's";
  EXPECT_EQ(gateway_->tip().value().block.height, 0u);

  source.beaconKey = testBeaconKey();
  source.alter = 1;
  EXPECT_FALSE(gateway_->poll().isOk()) << "a block that does not hash to the one the tip signs";
  EXPECT_EQ(gateway_->tip().value().block.height, 0u);

  source.alter.reset();
  poll();
  EXPECT_EQ(gateway_->tip().value().block.height, 1u);
}

TEST_F(GatewayTest, ResumesFromItsJournal) {
  ChainSource source(harness_.producer);
  open(source);
  gateway_->setClockOverride(slotTime(0));
  poll();
  ASSERT_TRUE(gateway_->watch(kUser).isOk());
  add(1, {newUser(kUser, 1000)});
  poll();
  add(2, {transfer(kUser, 10)});
  poll();

  open(source);
  gateway_->setClockOverride(slotTime(0));
  EXPECT_EQ(gateway_->tip().error().code, Gateway::kUnavailable) << "block 0 is checked first";
  poll();
  EXPECT_EQ(gateway_->tip().value().block.height, 2u);
  EXPECT_EQ(gateway_->watched(), std::vector<uint64_t>{kUser});
  EXPECT_EQ(balanceAt(kUser, height(1)), 1000);
  add(3, {transfer(kUser, 10)});
  poll();
  EXPECT_EQ(balanceAt(kUser, At{}), 1020);
  EXPECT_EQ(balanceAt(kUser, height(2)), 1010);

  // A crash mid-write leaves a partial entry: dropped.
  gateway_.reset();
  {
    std::ofstream out(dataDir_ / "journal.bin", std::ios::binary | std::ios::app);
    out.write("\x40\x00\x00\x00partial", 11);
  }
  open(source);
  gateway_->setClockOverride(slotTime(0));
  poll();
  EXPECT_EQ(gateway_->tip().value().block.height, 3u);

  // Another chain's genesis: refused.
  gateway_.reset();
  Gateway::Options options;
  options.genesisHash = std::string(32, '\x02');
  options.dataDir = dataDir_;
  Gateway other(source, options);
  EXPECT_FALSE(other.open().isOk());
}

TEST_F(GatewayTest, ForgetsOldFinalBlocksAndRewritesItsJournal) {
  ChainSource source(harness_.producer);
  Gateway::Options options;
  options.genesisHash = harness_.genesis.hash;
  options.dataDir = dataDir_;
  options.keepFinalSeconds = 3600;
  options.compactAfterBytes = 1; // every round
  gateway_ = std::make_unique<Gateway>(source, options);
  ASSERT_TRUE(gateway_->open().isOk());
  gateway_->setClockOverride(slotTime(0));
  poll();
  ASSERT_TRUE(gateway_->watch(kUser).isOk());
  add(1, {newUser(kUser, 1000)});
  poll();
  add(2, {transfer(kUser, 10)});
  poll();
  add(3, {transfer(AccountBuffer::ID_REGISTRAR, 10)});
  // Blocks 0 to 2 are final (an hour after their slots), block 3 not yet; blocks before slot 2's
  // start plus 2 s are past the hour kept.
  const int64_t later = slotTime(2) + 3600 + 2;
  gateway_->setClockOverride(later);
  poll();
  EXPECT_EQ(gateway_->tip().value().finalizedHeight, std::optional<uint64_t>(2));
  EXPECT_EQ(gateway_->blockAt(1).error().code, Gateway::kNotFound) << "forgotten";
  EXPECT_TRUE(gateway_->blockAt(2).isOk()) << "the newest final one past the retention starts what is kept";
  EXPECT_EQ(balanceAt(kUser, height(2)), 1010);
  EXPECT_EQ(balanceAt(kUser, At{}), 1010);
  const auto size = std::filesystem::file_size(dataDir_ / "journal.bin");

  gateway_ = std::make_unique<Gateway>(source, options);
  ASSERT_TRUE(gateway_->open().isOk());
  gateway_->setClockOverride(later);
  poll();
  EXPECT_EQ(gateway_->tip().value().block.height, 3u);
  EXPECT_EQ(balanceAt(kUser, At{At::Final}), 1010);
  EXPECT_EQ(gateway_->blockAt(1).error().code, Gateway::kNotFound);
  EXPECT_LE(std::filesystem::file_size(dataDir_ / "journal.bin"), size + 64) << "rewritten, not grown";
}

TEST_F(GatewayTest, ATakeoverRewindsWhatWasAboveItsCutover) {
  ChainSource source(harness_.producer);
  open(source);
  gateway_->setClockOverride(slotTime(0));
  poll();
  ASSERT_TRUE(gateway_->watch(kUser).isOk());
  const Ledger::ChainNode block1 = add(1, {transfer(AccountBuffer::ID_REGISTRAR, 10)});
  add(2, {newUser(kUser, 1000)});
  add(3, {transfer(kUser, 10)});
  gateway_->setClockOverride(slotTime(3) + 3600);
  poll();
  EXPECT_EQ(gateway_->tip().value().finalizedHeight, std::optional<uint64_t>(3));
  EXPECT_EQ(balanceAt(kUser, At{At::Final}), 1010);

  // The admins certify generation 2 from block 1 on; its chain (the peer's) goes another way.
  auto &peer = harness_.peer;
  ASSERT_TRUE(peer.addBlock(block1).isOk());
  const auto newKey = makeKeyPair();
  BeaconCertificate cert = harness_.producer.getAuthority().value().beacon;
  cert.generation = 2;
  cert.publicKey = newKey.publicKey;
  cert.cutoverHeight = 1;
  cert.cutoverHash = block1.hash;
  Ledger::TxBeaconCert certTx;
  certTx.meta = cert.ltsToString();
  certTx.idempotentId = 100;
  certTx.validationTsMin = harness_.chainConfig.genesisTime;
  certTx.validationTsMax = harness_.chainConfig.genesisTime + 3600;
  const Ledger::Record certRecord =
      makeRecord(Ledger::T_BEACON_CERT, certTx, harness_.genesisKey, peer.getNetworkId());
  const Ledger::ChainNode block2b = makeNextBlockAtSlot(peer, block1, 4, {certRecord});
  auto added = peer.addBlock(block2b);
  ASSERT_TRUE(added.isOk()) << added.error().message;

  ChainSource taken(peer);
  taken.beaconKey = newKey;
  taken.generation = 2;
  taken.certificate = certRecord;
  open(taken); // the service now reads a node on the new generation
  gateway_->setClockOverride(slotTime(3) + 3600);
  poll();

  auto tip = gateway_->tip().value();
  EXPECT_EQ(tip.block.height, 2u);
  EXPECT_EQ(tip.block.hash, block2b.hash);
  EXPECT_EQ(tip.generation, 2u);
  auto events = gateway_->events(0, 10);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, Event::Certificate);
  EXPECT_EQ(events[0].generation, 2u);
  EXPECT_EQ(events[1].kind, Event::Rewind);
  EXPECT_EQ(events[1].height, 1u);
  EXPECT_EQ(events[1].droppedUpTo, 3u);
  EXPECT_TRUE(events[1].crossedFinality) << "blocks 2 and 3 were final";
  EXPECT_EQ(gateway_->events(events[0].seq, 10).size(), 1u);
  EXPECT_EQ(balanceAt(kUser, At{}), -1) << "the account the dropped blocks made is not on this chain";
}

TEST_F(GatewayTest, AnEquivocationHaltsItAtItsHeight) {
  ChainSource source(harness_.producer);
  open(source);
  poll();
  add(1, {transfer(AccountBuffer::ID_REGISTRAR, 10)});
  poll();
  Client::SignedTip other = source.signTip(1);
  other.blockHash = std::string(32, '\x07');
  other.signature = utl::mlDsaSign(testBeaconKey().privateKey, other.signingMessage()).value();
  source.equivocation = Client::Equivocation::of(source.signTip(1), other);
  ASSERT_TRUE(source.equivocation.has_value());
  add(2, {transfer(AccountBuffer::ID_REGISTRAR, 10)});
  EXPECT_FALSE(gateway_->poll().isOk()) << "its tip is above the equivocation";
  auto tip = gateway_->tip().value();
  EXPECT_EQ(tip.block.height, 1u);
  EXPECT_FALSE(tip.halted.empty());
  auto events = gateway_->events(0, 10);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, Event::Equivocation);
  EXPECT_EQ(events[0].height, 1u);
}

TEST_F(GatewayTest, FinalityComesFromTheHeadersAlone) {
  auto &producer = harness_.producer;
  const auto &cfg = harness_.chainConfig; // 5 s slots, 10 an epoch, an hour
  Chain::GenesisAccountMeta gm;
  gm.config = cfg;
  gm.config.finalitySeconds = 7200;
  gm.genesis.wallet = producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
  gm.genesis.wallet.publicKeys = {makeKeyPair().publicKey, makeKeyPair().publicKey, makeKeyPair().publicKey};
  gm.genesis.wallet.minSignatures = 2;
  gm.genesis.meta = AccountAttachment::emptySerialized();
  gm.authority = producer.getAuthority().value();
  Ledger::TxConfig tx;
  tx.meta = gm.ltsToString();
  tx.idempotentId = 1;
  tx.validationTsMin = cfg.genesisTime;
  tx.validationTsMax = cfg.genesisTime + 3600;
  add(1, {makeRecord(Ledger::T_CONFIG, tx, harness_.genesisKey, producer.getNetworkId())}); // two hours from epoch 1
  const Ledger::ChainNode block2 = add(cfg.slotsPerEpoch, {transfer(AccountBuffer::ID_REGISTRAR, 10)});
  ASSERT_EQ(block2.block.finalAt, slotTime(cfg.slotsPerEpoch) + 7200);

  // Started at block 2, with a node that hides the config update from its authority proof and reports
  // a bound of a minute: the header says two hours, and only the header counts.
  struct HidingSource : ChainSource {
    using ChainSource::ChainSource;
    Roe<Client::AuthorityProof> authorityProof(uint64_t) override { return Client::AuthorityProof{}; }
  } source(producer);
  source.reportedFinality = 60;
  open(source);
  gateway_->setClockOverride(slotTime(cfg.slotsPerEpoch) + 3600);
  poll();
  EXPECT_EQ(gateway_->tip().value().finalizedHeight, std::nullopt) << "an hour is not enough under two";
  EXPECT_EQ(gateway_->blockAt(2).value().finalityTime, block2.block.finalAt);
  gateway_->setClockOverride(slotTime(cfg.slotsPerEpoch) + 7199);
  poll();
  EXPECT_EQ(gateway_->tip().value().finalizedHeight, std::nullopt);
  gateway_->setClockOverride(slotTime(cfg.slotsPerEpoch) + 7200);
  poll();
  EXPECT_EQ(gateway_->tip().value().finalizedHeight, std::optional<uint64_t>(2)) << "and every block below it";
  EXPECT_EQ(gateway_->blockAt(1).error().code, Gateway::kNotFound) << "not held";
}

} // namespace
} // namespace pp::gateway
