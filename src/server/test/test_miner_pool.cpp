#include "BeaconServer.h"
#include "Beacon.h"
#include "Miner.h"
#include "Rewind.h"
#include "../../chain/AccountBuffer.h"
#include "../../ledger/Ledger.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Utilities.h"
#include "TestTempPath.h"

#include <gtest/gtest.h>
#include <iterator>
#include <cctype>
#include <algorithm>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

using namespace pp;

namespace {

namespace fs = std::filesystem;

/**
 * Fresh beacon genesis (1 s slots) with one genesis miner, and a Miner running
 * as it. System accounts never lead, so it is the whole committee; tests still
 * wait for slots it leads. Transactions are reserve -> fee transfers.
 */
class MinerPoolTest : public ::testing::Test {
protected:
  static constexpr uint64_t kMinerId = AccountBuffer::ID_FIRST_ISSUED;

  void SetUp() override {
    root_ = pp::test::uniqueTempPath("pp-ledger-miner-pool-test");
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_ / "beacon");
    auto minerKey = utl::mlDsaGenerate();
    ASSERT_TRUE(minerKey.isOk());
    // Each system account's holder makes its own key; genesis signs genesis.
    // The genesis (admin) account is 2-of-3; two of its keys sign genesis.
    genesisKeys_ = {utl::mlDsaGenerate().value(), utl::mlDsaGenerate().value(), utl::mlDsaGenerate().value()};
    const auto &genesisKeys = genesisKeys_;
    reserveKeys_ = {utl::mlDsaGenerate().value()};
    auto pub = [](const utl::MlDsaKeyPair &k) { return R"({"publicKeys": [")" + utl::hexEncode(k.publicKey) + R"("]})"; };
    std::vector<std::string> genesisKeyFiles;
    std::string genesisPubs;
    for (size_t i = 0; i < genesisKeys.size(); ++i) {
      genesisPubs += (i ? R"(", ")" : "") + utl::hexEncode(genesisKeys[i].publicKey);
      if (i < 2) {
        genesisKeyFiles.push_back((root_ / ("genesis" + std::to_string(i) + ".key")).string());
        std::ofstream(genesisKeyFiles.back()) << utl::hexEncode(genesisKeys[i].privateKey) << "\n";
      }
    }
    std::ofstream(root_ / "beacon" / "init-config.json")
        << R"({"networkId": "test-net", "systemAccounts": {"genesis": {"publicKeys": [")" << genesisPubs
        << R"("], "minSignatures": 2})"
        << R"(, "fee": )" << pub(utl::mlDsaGenerate().value()) << R"(, "reserve": )" << pub(reserveKeys_[0])
        << R"(, "registrar": )" << pub(utl::mlDsaGenerate().value())
        << R"(, "recycle": )" << pub(utl::mlDsaGenerate().value()) << "},"
        << R"( "recovery": )" << pub(utl::mlDsaGenerate().value()) << ","
        << R"( "slotDuration": 1, "slotsPerEpoch": 1000, "maxCustomMetaSize": 10000,)"
        << R"( "maxTransactionsPerBlock": 100, "minFeeCoefficients": [1, 1, 0],)"
        << R"( "freeCustomMetaSize": 1024, "checkpointMinBlocks": 1000,)"
        << R"( "checkpointMinAgeSeconds": 0, "heartbeatSlots": 1000,)"
        << R"( "genesisMiners": [{"id": )" << kMinerId << R"(, "publicKeys": [")"
        << utl::hexEncode(minerKey.value().publicKey) << R"("]}]})";

    {
      BeaconServer beacon;
      auto init = beacon.init((root_ / "beacon").string(), genesisKeyFiles);
      ASSERT_TRUE(init.isOk()) << init.error().message;
    }

    Ledger ledger;
    ASSERT_TRUE(ledger.mount((root_ / "beacon" / "data" / "ledger").string()).isOk());
    auto genesis = ledger.readBlock(0);
    ASSERT_TRUE(genesis.isOk()) << genesis.error().message;

    miner_ = std::make_unique<Miner>();
    Miner::InitConfig config;
    config.workDir = (root_ / "miner").string();
    config.minerId = kMinerId;
    config.privateKeys.push_back(minerKey.value().privateKey);
    ASSERT_TRUE(miner_->init(config).isOk());
    ASSERT_TRUE(miner_->addBlock(genesis.value()).isOk());
    miner_->refresh();
  }

  void TearDown() override {
    miner_.reset();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  /** Reserve → fee transfer valid only during `slot`. */
  Ledger::Record transferValidOnlyIn(uint64_t slot, uint64_t idempotentId) {
    Ledger::TxDefault tx;
    tx.tokenId = AccountBuffer::ID_GENESIS;
    tx.fromWalletId = AccountBuffer::ID_RESERVE;
    tx.toWalletId = AccountBuffer::ID_FEE;
    tx.amount = 10;
    tx.fee = 100000;
    tx.idempotentId = idempotentId;
    tx.validationTsMin = miner_->getSlotStartTime(slot);
    tx.validationTsMax = miner_->getSlotStartTime(slot);
    Ledger::Record rec;
    rec.type = Ledger::T_DEFAULT;
    rec.data = utl::binaryPack(tx);
    for (const auto &k : reserveKeys_) {
      auto sig = utl::mlDsaSign(k.privateKey, rec.signingMessage("test-net"));
      EXPECT_TRUE(sig.isOk());
      rec.signatures.push_back(sig.value());
    }
    return rec;
  }

  /** Reserve → fee transfer valid from `slot` for `seconds`. */
  Ledger::Record transferValidFrom(uint64_t slot, int64_t seconds, uint64_t idempotentId) {
    Ledger::Record rec = transferValidOnlyIn(slot, idempotentId);
    auto tx = utl::binaryUnpack<Ledger::TxDefault>(rec.data);
    EXPECT_TRUE(tx.isOk());
    tx.value().validationTsMax = tx.value().validationTsMin + seconds;
    rec.data = utl::binaryPack(tx.value());
    rec.signatures.clear();
    for (const auto &k : reserveKeys_) {
      auto sig = utl::mlDsaSign(k.privateKey, rec.signingMessage("test-net"));
      EXPECT_TRUE(sig.isOk());
      rec.signatures.push_back(sig.value());
    }
    return rec;
  }

  /** Poll produceBlock as the run loop does until it seals (its slot ended); false on timeout. */
  bool produceWhenSealTime(Ledger::ChainNode &block) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until) {
      miner_->refresh();
      auto produced = miner_->produceBlock(block);
      EXPECT_TRUE(produced.isOk()) << produced.error().message;
      if (!produced.isOk()) {
        return false;
      }
      if (produced.value()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  /** Wait for a slot after `after` that this miner leads; that slot, or nullopt on timeout. */
  std::optional<uint64_t> waitForLeaderSlotAfter(int64_t after) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < until) {
      miner_->refresh();
      const uint64_t slot = miner_->getProductionSlot();  // the slot it collects for / seals
      if (static_cast<int64_t>(slot) > after && miner_->isSlotLeader()) {
        return slot;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
  }

  fs::path root_;
  /** The admin set: 2-of-3. */
  std::vector<utl::MlDsaKeyPair> genesisKeys_;
  std::vector<utl::MlDsaKeyPair> reserveKeys_;
  std::unique_ptr<Miner> miner_;
};

// A pending transaction that stops applying (here: its validity window ended)
// must be dropped when the next slot rebuilds the pool — not abort the slot
// and stay pending, which stalled the leader for every later slot.
TEST_F(MinerPoolTest, StalePendingTransactionDoesNotStallTheLeader) {
  auto slot = waitForLeaderSlotAfter(-1);
  ASSERT_TRUE(slot.has_value());
  auto added = miner_->addTransaction(transferValidOnlyIn(*slot, 1));
  ASSERT_TRUE(added.isOk()) << added.error().message;
  ASSERT_EQ(miner_->getPendingTransactionCount(), 1u);

  // A later slot we lead rebuilds the pool; the transaction's window has ended.
  auto later = waitForLeaderSlotAfter(static_cast<int64_t>(*slot));
  ASSERT_TRUE(later.has_value());
  Ledger::ChainNode block;
  auto produced = miner_->produceBlock(block);
  ASSERT_TRUE(produced.isOk()) << produced.error().message;
  EXPECT_EQ(miner_->getPendingTransactionCount(), 0u);

  // And the leader keeps working: a fresh transaction is accepted.
  auto again = waitForLeaderSlotAfter(static_cast<int64_t>(*later) - 1);
  ASSERT_TRUE(again.has_value());
  auto fresh = miner_->addTransaction(transferValidOnlyIn(*again, 2));
  EXPECT_TRUE(fresh.isOk()) << fresh.error().message;
}

// A block that fails to broadcast is abandoned: its transactions go back to pending, while the
// beacon still holds them in its pool for the next pull. Pulling one again must be refused — two
// copies of one idempotency key can never be sealed, and the leader stalled for every later slot.
TEST_F(MinerPoolTest, AbandonedBlockThenRepulledTransactionStillSeals) {
  auto slot = waitForLeaderSlotAfter(-1);
  ASSERT_TRUE(slot.has_value());
  const Ledger::Record tx = transferValidFrom(*slot, 60, 7);
  auto added = miner_->addTransaction(tx);
  ASSERT_TRUE(added.isOk()) << added.error().message;

  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));
  ASSERT_EQ(block.block.records.size(), 1u);
  miner_->abandonBlock(block);  // broadcast failed
  ASSERT_EQ(miner_->getPendingTransactionCount(), 1u);

  auto again = waitForLeaderSlotAfter(static_cast<int64_t>(*slot) - 1);
  ASSERT_TRUE(again.has_value());
  auto repulled = miner_->addTransaction(tx);
  EXPECT_FALSE(repulled.isOk()) << "the same idempotency key is already in the draft";
  EXPECT_EQ(miner_->getPendingTransactionCount(), 1u);

  Ledger::ChainNode next;
  EXPECT_TRUE(produceWhenSealTime(next)) << "the leader must still seal";
  EXPECT_EQ(next.block.records.size(), 1u);
}

// beacon-authority ba8: a certificate held out of band leaves the disk as soon as a block commits
// it — not at the next start, when a stale file would be offered again only to be dropped.
TEST_F(MinerPoolTest, AHeldCertificateLeavesTheDiskOnceCommitted) {
  BeaconCertificate cert;
  cert.networkId = "test-net";
  cert.generation = 2;
  cert.publicKey = utl::mlDsaGenerate().value().publicKey;
  cert.cutoverHeight = UINT64_MAX; // none: generation 1 signs until it commits
  Ledger::TxBeaconCert tx;
  tx.meta = cert.ltsToString();
  tx.idempotentId = 9;
  tx.validationTsMin = miner_->getConsensusTimestamp() - 60;
  tx.validationTsMax = miner_->getConsensusTimestamp() + 3600;
  Ledger::Record record;
  record.type = Ledger::T_BEACON_CERT;
  record.data = utl::binaryPack(tx);
  for (size_t i = 0; i < 2; ++i) {
    record.signatures.push_back(utl::mlDsaSign(genesisKeys_[i].privateKey, record.signingMessage("test-net")).value());
  }
  const fs::path file = root_ / "miner" / "beacon-cert.dat";
  ASSERT_EQ(miner_->noteCertificate(record, "a test"), std::optional<Chain::CertificateOffer>(Chain::CertificateOffer::Newer));
  ASSERT_TRUE(fs::exists(file));

  ASSERT_TRUE(waitForLeaderSlotAfter(-1).has_value());
  auto added = miner_->addTransaction(record);
  ASSERT_TRUE(added.isOk()) << added.error().message;
  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));
  ASSERT_EQ(block.block.records.size(), 1u);
  auto committed = miner_->addBlock(block);
  ASSERT_TRUE(committed.isOk()) << committed.error().message;
  EXPECT_FALSE(miner_->getSeenCertificate().has_value()) << "generation 2 is on chain";
  EXPECT_FALSE(fs::exists(file)) << "and its file is gone";
}

// The beacon signs what it commits, so every block it adds is durable before it returns: a crash
// right after keeps it (relays and miners commit in batches and sync what a crash loses).
TEST_F(MinerPoolTest, EveryBlockTheBeaconAddsSurvivesACrash) {
  auto slot = waitForLeaderSlotAfter(-1);
  ASSERT_TRUE(slot.has_value());
  ASSERT_TRUE(miner_->addTransaction(transferValidFrom(*slot, 60, 11)).isOk());
  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));

  Beacon beacon;
  Beacon::MountConfig mount;
  mount.workDir = (root_ / "beacon" / "data").string();
  ASSERT_TRUE(beacon.mount(mount).isOk());
  auto added = beacon.addBlock(block);
  ASSERT_TRUE(added.isOk()) << added.error().message;

  const fs::path crashed = root_ / "beacon-ledger-crashed";
  fs::copy(root_ / "beacon" / "data" / "ledger", crashed, fs::copy_options::recursive);
  Ledger restarted;
  ASSERT_TRUE(restarted.mount(crashed.string()).isOk());
  EXPECT_EQ(restarted.getNextBlockId(), 2u) << "genesis and the block just added";
}

// beacon-authority bp1: a beacon ordered to rewind applies it at its next start — the ledger is cut
// after the cutover, a tip it signed above it is dropped (that generation is superseded), and the
// rewind is reported.
TEST_F(MinerPoolTest, ABeaconAppliesARewindOrderAtItsNextStart) {
  auto slot = waitForLeaderSlotAfter(-1);
  ASSERT_TRUE(slot.has_value());
  ASSERT_TRUE(miner_->addTransaction(transferValidFrom(*slot, 60, 12)).isOk());
  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));

  const fs::path beaconDir = root_ / "beacon";
  auto signingKey = [&]() {
    utl::MlDsaKeyPair key;
    key.privateKey = utl::readPrivateKey("keys/beacon-signing.key", beaconDir.string()).value();
    std::ifstream pub(beaconDir / "keys" / "beacon-signing.pub");
    std::string hex((std::istreambuf_iterator<char>(pub)), std::istreambuf_iterator<char>());
    hex.erase(std::remove_if(hex.begin(), hex.end(), [](unsigned char c) { return std::isspace(c); }), hex.end());
    key.publicKey = utl::hexDecode(hex);
    return key;
  }();
  Beacon::MountConfig mount;
  mount.workDir = (beaconDir / "data").string();
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning({signingKey}).isOk());
    ASSERT_TRUE(beacon.addBlock(block).isOk());
    ASSERT_EQ(beacon.getSignedTip()->height, 1u);
  }
  Client::RewindReport order;
  order.keptUpTo = 0;
  order.droppedUpTo = 1;
  order.generation = 2;
  ASSERT_TRUE(rewind::writeOrder(mount.workDir, order).isOk());

  Beacon restarted;
  ASSERT_TRUE(restarted.mount(mount).isOk());
  EXPECT_EQ(restarted.getNextBlockId(), 1u) << "cut after the cutover";
  EXPECT_EQ(restarted.getLastRewind(), std::optional<Client::RewindReport>(order));
  ASSERT_TRUE(restarted.openTipSigning({signingKey}).isOk()) << "the tip above the cutover is gone";
  EXPECT_EQ(restarted.getSignedTip()->height, 0u);
}

// beacon-authority bp1: a node rewinds on its own only within kMaxAutomaticRewind of slot time; a
// deeper cut is held back — the node keeps refusing the old tips past it — until the operator
// approves that generation (--approve-rewind).
TEST_F(MinerPoolTest, ARewindDeeperThanTheNodeTakesAloneWaitsForApproval) {
  auto slot = waitForLeaderSlotAfter(-1);
  ASSERT_TRUE(slot.has_value());
  ASSERT_TRUE(miner_->addTransaction(transferValidFrom(*slot, 60, 13)).isOk());
  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));
  Beacon::MountConfig mount;
  mount.workDir = (root_ / "beacon" / "data").string();
  Beacon beacon;
  ASSERT_TRUE(beacon.mount(mount).isOk());
  ASSERT_TRUE(beacon.addBlock(block).isOk());

  BeaconCertificate cert; // generation 2, cutover at genesis: block 1 is past it
  cert.networkId = "test-net";
  cert.generation = 2;
  cert.publicKey = utl::mlDsaGenerate().value().publicKey;
  cert.cutoverHeight = 0;
  cert.cutoverHash = beacon.readBlock(0).value().hash;
  Ledger::TxBeaconCert tx;
  tx.meta = cert.ltsToString();
  Ledger::Record record;
  record.type = Ledger::T_BEACON_CERT;
  record.data = utl::binaryPack(tx);
  for (size_t i = 0; i < 2; ++i) {
    record.signatures.push_back(utl::mlDsaSign(genesisKeys_[i].privateKey, record.signingMessage("test-net")).value());
  }
  ASSERT_EQ(beacon.offerCertificate(record).value(), Chain::CertificateOffer::Newer);
  const fs::path order = fs::path(mount.workDir) / rewind::FILE_ORDER;

  beacon.setMaxAutomaticRewind(std::chrono::seconds(0)); // block 1 is a slot or more past genesis
  EXPECT_FALSE(beacon.scheduleRewind(0).value()) << "held back";
  EXPECT_FALSE(fs::exists(order));
  EXPECT_FALSE(beacon.scheduleRewind(3).value()) << "an approval for another generation does not count";
  EXPECT_TRUE(beacon.scheduleRewind(2).value()) << "approved";
  EXPECT_TRUE(fs::exists(order));
  fs::remove(order);

  beacon.setMaxAutomaticRewind(rewind::kMaxAutomaticRewind);
  EXPECT_TRUE(beacon.scheduleRewind(0).value()) << "within seven days: on its own";
  EXPECT_TRUE(fs::exists(order));
}

// beacon-authority bp2: a certificate vouching for another block at the cutover than the node holds
// orders no rewind — the node's history forked at or below it, or the certificate names the wrong
// block (a typo'd height must not cut more): the node halts and alerts instead.
TEST_F(MinerPoolTest, ACertificateVouchingForAnotherCutoverBlockIsNotObeyed) {
  auto slot = waitForLeaderSlotAfter(-1);
  ASSERT_TRUE(slot.has_value());
  ASSERT_TRUE(miner_->addTransaction(transferValidFrom(*slot, 60, 14)).isOk());
  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));
  Beacon::MountConfig mount;
  mount.workDir = (root_ / "beacon" / "data").string();
  Beacon beacon;
  ASSERT_TRUE(beacon.mount(mount).isOk());
  ASSERT_TRUE(beacon.addBlock(block).isOk());

  BeaconCertificate cert; // generation 2, cutover at genesis — with a hash that is not genesis's
  cert.networkId = "test-net";
  cert.generation = 2;
  cert.publicKey = utl::mlDsaGenerate().value().publicKey;
  cert.cutoverHeight = 0;
  cert.cutoverHash = std::string(32, 'x');
  Ledger::TxBeaconCert tx;
  tx.meta = cert.ltsToString();
  Ledger::Record record;
  record.type = Ledger::T_BEACON_CERT;
  record.data = utl::binaryPack(tx);
  for (size_t i = 0; i < 2; ++i) {
    record.signatures.push_back(utl::mlDsaSign(genesisKeys_[i].privateKey, record.signingMessage("test-net")).value());
  }
  ASSERT_EQ(beacon.offerCertificate(record).value(), Chain::CertificateOffer::Newer);
  EXPECT_FALSE(beacon.scheduleRewind(0).value()) << "not on its own";
  EXPECT_FALSE(beacon.scheduleRewind(2).value()) << "nor approved: an approval does not fix a fork";
  EXPECT_FALSE(fs::exists(fs::path(mount.workDir) / rewind::FILE_ORDER));
}

} // namespace
