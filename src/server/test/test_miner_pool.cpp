#include "ledger/server/BeaconServer.h"
#include "ledger/server/Beacon.h"
#include "ledger/server/Miner.h"
#include "ledger/server/Rewind.h"
#include "ledger/chain/PredecessorCheck.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/StateSnapshot.h"
#include "ledger/client/AccountAttachment.h"
#include <map>
#include "ledger/ledger/Ledger.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"
#include "ledger/testing/TestTempPath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <iterator>
#include <cctype>
#include <algorithm>

#include <chrono>
#include <iostream>
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
    minerKey_ = minerKey.value();
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
        << R"( "maxTransactionsPerBlock": )" << maxTransactionsPerBlock_ << R"(, "minFeeCoefficients": [1, 1, 0],)"
        << R"( "freeCustomMetaSize": 1024, "checkpointMinBlocks": )" << checkpointMinBlocks_ << ","
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

  /** Genesis settings a derived fixture may change before SetUp. */
  uint64_t maxTransactionsPerBlock_ = 100;
  uint64_t checkpointMinBlocks_ = 1000;

  utl::MlDsaKeyPair minerKey_;
  fs::path root_;
  /** The admin set: 2-of-3. */
  std::vector<utl::MlDsaKeyPair> genesisKeys_;
  std::vector<utl::MlDsaKeyPair> reserveKeys_;
  std::unique_ptr<Miner> miner_;
};

/** Accounts fall due for renewal three blocks after they are written, three records a block. */
class LeaderBudgetTest : public MinerPoolTest {
protected:
  LeaderBudgetTest() {
    maxTransactionsPerBlock_ = 3;
    checkpointMinBlocks_ = 3;
  }
};

bool isRenewal(const Ledger::Record &record) {
  return record.type == Ledger::T_RENEWAL || record.type == Ledger::T_END_USER;
}

// B035: the leader packs every block to the limits. The six accounts written in genesis fall due
// together in block 3, twice what a block may hold: the leader renews three, leaves the rest for the
// next block, and keeps its pending transfer out while renewals wait; once they are drained the
// transfer goes in. Every block it makes is one the chain accepts.
TEST_F(LeaderBudgetTest, DueRenewalsBeyondABlockWaitAndTransactionsWaitForThem) {
  std::vector<Ledger::ChainNode> blocks;
  uint64_t idempotentId = 1;
  int64_t after = -1;
  bool transferAfterRenewals = false;
  while (!transferAfterRenewals && blocks.size() < 12) {
    auto slot = waitForLeaderSlotAfter(after);
    ASSERT_TRUE(slot.has_value());
    auto added = miner_->addTransaction(transferValidFrom(*slot, 60, idempotentId++));
    ASSERT_TRUE(added.isOk()) << added.error().message;
    Ledger::ChainNode block;
    ASSERT_TRUE(produceWhenSealTime(block));
    auto committed = miner_->addBlock(block);
    ASSERT_TRUE(committed.isOk()) << "block " << block.block.index << ": " << committed.error().message;
    miner_->markBlockProduction(block);
    after = static_cast<int64_t>(block.block.slot);
    ASSERT_LE(block.block.records.size(), 3u) << "block " << block.block.index;
    const bool renewsAny = std::any_of(block.block.records.begin(), block.block.records.end(), isRenewal);
    const bool renewalsSeen = std::any_of(blocks.begin(), blocks.end(), [](const Ledger::ChainNode &b) {
      return std::any_of(b.block.records.begin(), b.block.records.end(), isRenewal);
    });
    transferAfterRenewals = renewalsSeen && !std::all_of(block.block.records.begin(), block.block.records.end(), isRenewal);
    if (renewsAny || transferAfterRenewals) {
      blocks.push_back(block);
    } else {
      blocks.clear(); // before the renewals fall due
      blocks.push_back(block);
    }
  }
  ASSERT_TRUE(transferAfterRenewals) << "the transfer never got in after the renewals";
  // blocks: the last block before renewals, the renewals-only blocks, then the one with the transfer.
  ASSERT_GE(blocks.size(), 4u);
  std::set<uint64_t> renewed;
  for (size_t i = 1; i + 1 < blocks.size(); ++i) {
    const auto &records = blocks[i].block.records;
    EXPECT_EQ(records.size(), 3u) << "block " << blocks[i].block.index << " is filled to the limit";
    for (const auto &record : records) {
      ASSERT_TRUE(isRenewal(record)) << "block " << blocks[i].block.index << ": renewals only while some wait";
      auto tx = utl::binaryUnpack<Ledger::TxRenewal>(record.data);
      ASSERT_TRUE(tx.isOk());
      renewed.insert(tx.value().walletId);
    }
  }
  EXPECT_GE(renewed.size(), 6u) << "every account written in genesis renewed";
}

/** Ten records a block; an account comes due for renewal ten blocks after it was written. */
class RenewalEchoTest : public MinerPoolTest {
protected:
  RenewalEchoTest() {
    maxTransactionsPerBlock_ = 10;
    checkpointMinBlocks_ = 10;
  }

  /** A new account from the reserve, valid for two minutes from `slot`. */
  Ledger::Record newAccount(uint64_t id, uint64_t slot, uint64_t idempotentId) {
    auto keys = utl::mlDsaGenerate();
    EXPECT_TRUE(keys.isOk());
    Client::UserAccount account;
    account.wallet.publicKeys = {keys.value().publicKey};
    account.wallet.minSignatures = 1;
    account.wallet.keyType = Crypto::TK_ML_DSA_65;
    account.wallet.mBalances[AccountBuffer::ID_GENESIS] = 1000;
    account.meta = AccountAttachment::emptySerialized();
    Ledger::TxNewUser tx;
    tx.fromWalletId = AccountBuffer::ID_RESERVE;
    tx.toWalletId = id;
    tx.amount = 1000;
    tx.fee = 100000;
    tx.meta = account.ltsToString();
    tx.idempotentId = idempotentId;
    tx.validationTsMin = miner_->getSlotStartTime(slot);
    tx.validationTsMax = tx.validationTsMin + 120;
    Ledger::Record rec;
    rec.type = Ledger::T_NEW_USER;
    rec.data = utl::binaryPack(tx);
    for (const auto &k : reserveKeys_) {
      rec.signatures.push_back(utl::mlDsaSign(k.privateKey, rec.signingMessage("test-net")).value());
    }
    return rec;
  }
};

// LR004 (load-resilience lr9): accounts written at block capacity for a while come due together a
// renewal period later, and — renewals keeping their priority (B035) — hold user transactions for
// about as long: here 40 accounts written over four blocks of ten fall due over four blocks ten
// blocks later, which carry renewals only while a transfer waits; then it goes in. The echo is the
// length of the burst, not more.
TEST_F(RenewalEchoTest, ABurstOfAccountsComesDueTogetherAndHoldsTransfersAsLong) {
  constexpr size_t kBurst = 40;
  std::vector<Ledger::Record> burst;
  std::set<std::string> burstData;
  uint64_t idempotentId = 1;
  int64_t after = -1;
  std::optional<Ledger::Record> waiting; // the transfer offered in every slot until it goes in
  uint64_t waitingSince = 0;
  uint64_t longestWait = 0;
  size_t renewalOnlyRun = 0;
  size_t longestRenewalOnlyRun = 0;
  size_t burstWritten = 0;
  uint64_t burstDoneAt = 0; // the block that wrote the last of the burst
  // On past the echo: the burst's renewals fall due ten blocks after it, over as many blocks as it took.
  for (int b = 0; b < 45 && (burstDoneAt == 0 || after < 0 ||
                              miner_->getNextBlockId() <= burstDoneAt + checkpointMinBlocks_ + 8);
       ++b) {
    auto slot = waitForLeaderSlotAfter(after);
    ASSERT_TRUE(slot.has_value());
    if (burst.empty()) {
      for (size_t i = 0; i < kBurst; ++i) {
        burst.push_back(newAccount((1ULL << 30) + 500 + i, *slot, idempotentId++));
        burstData.insert(burst.back().data);
      }
    }
    // Offered again each slot, as a client would (this harness has no beacon pool keeping them).
    for (const auto &record : burst) {
      if (burstData.count(record.data) > 0) {
        (void)miner_->addTransaction(record);
      }
    }
    if (burstWritten == kBurst && !waiting) {
      waiting = transferValidFrom(*slot, 120, idempotentId++);
    }
    if (waiting) {
      (void)miner_->addTransaction(*waiting);
    }
    Ledger::ChainNode block;
    ASSERT_TRUE(produceWhenSealTime(block));
    auto committed = miner_->addBlock(block);
    ASSERT_TRUE(committed.isOk()) << "block " << block.block.index << ": " << committed.error().message;
    miner_->markBlockProduction(block);
    after = static_cast<int64_t>(block.block.slot);
    if (waiting && waitingSince == 0) {
      waitingSince = block.block.index;
    }
    size_t renewals = 0;
    bool waitingIn = false;
    for (const auto &record : block.block.records) {
      renewals += isRenewal(record) ? 1 : 0;
      if (burstData.erase(record.data) > 0) {
        ++burstWritten;
      }
      waitingIn = waitingIn || (waiting && record.data == waiting->data);
    }
    if (burstWritten == kBurst && burstDoneAt == 0) {
      burstDoneAt = block.block.index;
    }
    renewalOnlyRun = renewals == block.block.records.size() && renewals > 0 ? renewalOnlyRun + 1 : 0;
    longestRenewalOnlyRun = std::max(longestRenewalOnlyRun, renewalOnlyRun);
    if (waitingIn) {
      longestWait = std::max<uint64_t>(longestWait, block.block.index - waitingSince + 1);
      waiting.reset();
      waitingSince = 0;
    }
  }
  ASSERT_EQ(burstWritten, kBurst);
  std::cout << "renewal echo: " << longestRenewalOnlyRun << " blocks of renewals only; a transfer waited "
            << longestWait << " blocks\n";
  // Four blocks' worth fall due (and the genesis accounts' first renewals near them): a few blocks of
  // renewals only, the transfer held about that long — no more than the burst and its neighbours.
  EXPECT_GE(longestRenewalOnlyRun, 3u) << "the burst came due together";
  EXPECT_LE(longestRenewalOnlyRun, 6u);
  EXPECT_LE(longestWait, longestRenewalOnlyRun + 2) << "the transfer went in once the renewals drained";
}

// A pending transaction that stops applying (here: its validity window ended)
// must be dropped when the next slot rebuilds the pool — not abort the slot
// and stay pending, which stalled the leader for every later slot.
// load-resilience lr4: what a pool checks before it holds a record — cheap, on the committed state.
TEST_F(MinerPoolTest, AdmissionRefusesWhatCouldNeverApply) {
  const uint64_t slot = miner_->getCurrentSlot();
  const int64_t from = miner_->getSlotStartTime(slot);
  auto valid = transferValidFrom(slot, 60, 21);
  EXPECT_TRUE(miner_->checkAdmission(valid, from, true)) << "signed by the reserve's keys, funded, in its window";
  const auto key = miner_->idempotencyKeyOf(valid);
  ASSERT_TRUE(key);
  EXPECT_EQ(key->first, AccountBuffer::ID_RESERVE);
  EXPECT_EQ(key->second, 21u);

  auto junk = valid;
  junk.signatures.front()[0] = static_cast<char>(junk.signatures.front()[0] ^ 0x5a);
  EXPECT_FALSE(miner_->checkAdmission(junk, from, true)) << "a bad signature";
  EXPECT_TRUE(miner_->checkAdmission(junk, from, false)) << "signatures are checked only when asked (once, on the way in)";

  EXPECT_FALSE(miner_->checkAdmission(valid, from + 61, true)) << "its window ended";

  auto rich = utl::binaryUnpack<Ledger::TxDefault>(valid.data).value();
  rich.amount = static_cast<uint64_t>(AccountIds::INITIAL_TOKEN_SUPPLY) * 2;
  Ledger::Record unfunded = valid;
  unfunded.data = utl::binaryPack(rich);
  EXPECT_FALSE(miner_->checkAdmission(unfunded, from, false)) << "the payer cannot cover it";

  auto stranger = utl::binaryUnpack<Ledger::TxDefault>(valid.data).value();
  stranger.fromWalletId = AccountIds::ID_FIRST_USER + 5; // no such account
  Ledger::Record nobody = valid;
  nobody.data = utl::binaryPack(stranger);
  EXPECT_FALSE(miner_->checkAdmission(nobody, from, true)) << "no account signed it";

  Ledger::Record garbage;
  garbage.type = Ledger::T_DEFAULT;
  garbage.data = "not a transaction";
  EXPECT_FALSE(miner_->checkAdmission(garbage, from, false));

  // An issued account (the genesis miner) issues its own token from nothing — the fee it pays in
  // the native token; another issuer's token it must hold.
  auto issue = utl::binaryUnpack<Ledger::TxDefault>(valid.data).value();
  issue.fromWalletId = kMinerId;
  issue.tokenId = kMinerId;
  issue.amount = 1'000'000;
  Ledger::Record issuing = valid;
  issuing.data = utl::binaryPack(issue);
  EXPECT_TRUE(miner_->checkAdmission(issuing, from, false)) << "an issuer issues its own token";
  issue.tokenId = kMinerId + 1;
  issuing.data = utl::binaryPack(issue);
  EXPECT_FALSE(miner_->checkAdmission(issuing, from, false)) << "another issuer's token, not held";
}

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

// beacon-authority bp4 (moving the beacon): the whole work directory copied after a clean stop runs
// on as the same beacon. Copied without data/signed-tip.dat, its ledger past genesis is not signed
// as found — it may be older than the tip the last host signed — unless the operator accepts it.
TEST_F(MinerPoolTest, ABeaconMovedWithoutItsSignedTipDoesNotSignItsLedgerUnasked) {
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
  Client::SignedTip signedTip;
  {
    Beacon beacon;
    Beacon::MountConfig mount;
    mount.workDir = (beaconDir / "data").string();
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning({signingKey}).isOk());
    ASSERT_TRUE(beacon.addBlock(block).isOk());
    signedTip = beacon.getSignedTip().value();
    ASSERT_EQ(signedTip.height, 1u);
  } // a clean stop

  auto moveTo = [&](const std::string &name, bool withTip) {
    const fs::path moved = root_ / name;
    fs::copy(beaconDir, moved, fs::copy_options::recursive);
    if (!withTip) {
      fs::remove(moved / "data" / "signed-tip.dat");
    }
    Beacon::MountConfig mount;
    mount.workDir = (moved / "data").string();
    return mount;
  };
  {
    Beacon moved; // the whole directory: the same beacon, the same tip
    ASSERT_TRUE(moved.mount(moveTo("moved", true)).isOk());
    ASSERT_TRUE(moved.openTipSigning({signingKey}).isOk());
    EXPECT_EQ(moved.getSignedTip(), std::optional<Client::SignedTip>(signedTip));
  }
  const auto withoutTip = moveTo("moved-without-tip", false);
  {
    Beacon moved;
    ASSERT_TRUE(moved.mount(withoutTip).isOk());
    auto refused = moved.openTipSigning({signingKey});
    ASSERT_FALSE(refused.isOk());
    EXPECT_NE(refused.error().message.find("--accept-ledger-tip"), std::string::npos) << refused.error().message;
    EXPECT_FALSE(fs::exists(fs::path(withoutTip.workDir) / "signed-tip.dat")) << "nothing signed";
  }
  {
    Beacon moved; // the operator knows this ledger holds every block the key signed
    ASSERT_TRUE(moved.mount(withoutTip).isOk());
    ASSERT_TRUE(moved.openTipSigning({signingKey}, /*acceptLedgerTip=*/true).isOk());
    EXPECT_EQ(moved.getSignedTip()->height, 1u);
    EXPECT_EQ(moved.getSignedTip()->blockHash, signedTip.blockHash);
  }
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

// beacon-authority bp3 (case 2): a relay promoted to be the new beacon holds the chain up to the
// cutover and the admins' certificate naming its key. It takes over — not standing down, signing
// nothing yet — and commits only the block after the cutover carrying that certificate, which the
// leader puts in its block itself; from there it signs as the new generation.
TEST_F(MinerPoolTest, APromotedRelayTakesOverAtTheCutover) {
  // The relay's directory: the chain up to block 0 (the cutover here), no signed tip.
  const fs::path relayData = root_ / "relay" / "data";
  fs::create_directories(relayData);
  fs::copy(root_ / "beacon" / "data" / "ledger", relayData / "ledger", fs::copy_options::recursive);
  Beacon::MountConfig mount;
  mount.workDir = relayData.string();
  Beacon promoted;
  ASSERT_TRUE(promoted.mount(mount).isOk());
  const utl::MlDsaKeyPair newKey = utl::mlDsaGenerate().value();
  ASSERT_TRUE(promoted.openTipSigning({newKey}).isOk());
  EXPECT_TRUE(promoted.supersededBy().has_value()) << "before the certificate: generation 1's key is not its";

  BeaconCertificate cert;
  cert.networkId = "test-net";
  cert.generation = 2;
  cert.publicKey = newKey.publicKey;
  cert.cutoverHeight = 0;
  cert.cutoverHash = promoted.readBlock(0).value().hash;
  Ledger::TxBeaconCert tx;
  tx.meta = cert.ltsToString(); // no validity window: a cutover certificate needs none
  Ledger::Record record;
  record.type = Ledger::T_BEACON_CERT;
  record.data = utl::binaryPack(tx);
  for (size_t i = 0; i < 2; ++i) {
    record.signatures.push_back(utl::mlDsaSign(genesisKeys_[i].privateKey, record.signingMessage("test-net")).value());
  }
  ASSERT_EQ(promoted.offerCertificate(record).value(), Chain::CertificateOffer::Newer);
  EXPECT_FALSE(promoted.supersededBy().has_value()) << "the certificate held names its key";
  EXPECT_EQ(promoted.takingOver(), std::optional<uint64_t>(2));
  EXPECT_FALSE(promoted.getSignedTip().has_value()) << "nothing signed before the cutover block";
  ASSERT_EQ(miner_->noteCertificate(record, "a test"), std::optional<Chain::CertificateOffer>(Chain::CertificateOffer::Newer));

  ASSERT_TRUE(waitForLeaderSlotAfter(-1).has_value());
  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));
  ASSERT_EQ(block.block.index, 1u);
  ASSERT_EQ(block.block.records.size(), 1u) << "the leader put the certificate in its block itself";
  EXPECT_EQ(block.block.records[0].type, Ledger::T_BEACON_CERT);

  Ledger::ChainNode without = block;
  without.block.records.clear();
  EXPECT_FALSE(promoted.addBlock(without).isOk()) << "nothing else commits above the cutover";

  auto added = promoted.addBlock(block);
  ASSERT_TRUE(added.isOk()) << added.error().message;
  EXPECT_EQ(promoted.getGeneration(), 2u);
  EXPECT_FALSE(promoted.takingOver().has_value()) << "taken over";
  ASSERT_TRUE(promoted.getSignedTip().has_value());
  EXPECT_EQ(promoted.getSignedTip()->generation, 2u);
  EXPECT_EQ(promoted.getSignedTip()->height, 1u);
  EXPECT_TRUE(promoted.getSignedTip()->verify(newKey.publicKey));
  ASSERT_TRUE(miner_->addBlock(block).isOk()) << "the miner commits it too";
  EXPECT_EQ(miner_->getGeneration(), 2u);
}

} // namespace

namespace {

/** A user account the reserve creates, with a self-published profile, valid from `slot`. */
Ledger::Record newUserWithProfile(Miner &miner, const utl::MlDsaKeyPair &reserve, uint64_t slot, uint64_t id,
                                  const utl::MlDsaKeyPair &userKey, uint64_t amount, const std::string &profile) {
  Client::UserAccount account;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = static_cast<int64_t>(amount);
  account.wallet.publicKeys = {userKey.publicKey};
  account.wallet.minSignatures = 1;
  account.wallet.keyType = Crypto::TK_ML_DSA_65;
  AccountAttachment attachment;
  attachment.profiles[id].data = profile;
  account.meta = attachment.ltsToString();
  Ledger::TxNewUser tx;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = id;
  tx.amount = amount;
  tx.fee = 100000;
  tx.meta = account.ltsToString();
  tx.idempotentId = 900;
  tx.validationTsMin = miner.getSlotStartTime(slot);
  tx.validationTsMax = tx.validationTsMin + 60;
  Ledger::Record rec;
  rec.type = Ledger::T_NEW_USER;
  rec.data = utl::binaryPack(tx);
  rec.signatures = {utl::mlDsaSign(reserve.privateKey, rec.signingMessage("test-net")).value()};
  return rec;
}

std::string pubJson(const utl::MlDsaKeyPair &k) {
  return R"({"publicKeys": [")" + utl::hexEncode(k.publicKey) + R"("]})";
}

} // namespace

// beacon-authority bp9 (case 1, re-genesis; B030, B032): a network's state, exported at its tip, is
// imported by a new network — new id, new genesis, admin, system and beacon keys. Every account comes
// over as it was (balances, keys, profile), but for the system accounts' keys; miners stay registered
// with fresh slot counters; the old config is carried but for what init-config.json sets. A miner of
// the old network syncs the import blocks — checking them against the old state root — and produces
// the next block with its old key.
TEST_F(MinerPoolTest, ARegenesisImportsTheOldStateAndTheNetworkGoesOn) {
  // The old network: a user with a profile, and a transfer.
  auto slot = waitForLeaderSlotAfter(-1);
  ASSERT_TRUE(slot.has_value());
  const uint64_t user = AccountBuffer::ID_FIRST_USER + 7;
  const auto userKey = utl::mlDsaGenerate().value();
  ASSERT_TRUE(miner_->addTransaction(newUserWithProfile(*miner_, reserveKeys_[0], *slot, user, userKey, 5000,
                                                        std::string(300, 'p')))
                  .isOk());
  ASSERT_TRUE(miner_->addTransaction(transferValidFrom(*slot, 60, 12)).isOk());
  Ledger::ChainNode block;
  ASSERT_TRUE(produceWhenSealTime(block));
  ASSERT_EQ(block.block.records.size(), 2u);
  ASSERT_TRUE(miner_->addBlock(block).isOk());
  {
    Beacon beacon; // the old beacon commits it
    Beacon::MountConfig mount;
    mount.workDir = (root_ / "beacon" / "data").string();
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.addBlock(block).isOk());
  }
  std::map<uint64_t, Client::UserAccount> before;
  for (uint64_t id : {AccountBuffer::ID_GENESIS, AccountBuffer::ID_RESERVE, AccountBuffer::ID_FEE, kMinerId, user}) {
    before[id] = miner_->getAccount(id).value();
  }
  ASSERT_GT(before[kMinerId].wallet.miner.lastProducedSlot, 0u) << "the miner produced on the old network";

  // Export at the tip; a new network imports it.
  auto snapshot = state_export::exportState((root_ / "beacon" / "data" / "ledger").string(), block.block.index,
                                            (root_ / "export-scratch").string());
  ASSERT_TRUE(snapshot.isOk()) << snapshot.error().message;
  const fs::path snapshotFile = root_ / "state.snap";
  std::ofstream(snapshotFile, std::ios::binary) << snapshot.value().ltsToString();

  const fs::path next = root_ / "beacon2";
  fs::create_directories(next);
  const std::vector<utl::MlDsaKeyPair> admins = {utl::mlDsaGenerate().value(), utl::mlDsaGenerate().value(),
                                                 utl::mlDsaGenerate().value()};
  const auto newReserve = utl::mlDsaGenerate().value();
  std::vector<std::string> adminKeyFiles;
  for (size_t i = 0; i < 2; ++i) {
    adminKeyFiles.push_back((root_ / ("admin" + std::to_string(i) + ".key")).string());
    std::ofstream(adminKeyFiles.back()) << utl::hexEncode(admins[i].privateKey) << "\n";
  }
  std::ofstream(next / "init-config.json")
      << R"({"networkId": "test-net-2", "systemAccounts": {"genesis": {"publicKeys": [")"
      << utl::hexEncode(admins[0].publicKey) << R"(", ")" << utl::hexEncode(admins[1].publicKey) << R"(", ")"
      << utl::hexEncode(admins[2].publicKey) << R"("], "minSignatures": 2})"
      << R"(, "fee": )" << pubJson(utl::mlDsaGenerate().value()) << R"(, "reserve": )" << pubJson(newReserve)
      << R"(, "registrar": )" << pubJson(utl::mlDsaGenerate().value())
      << R"(, "recycle": )" << pubJson(utl::mlDsaGenerate().value()) << "},"
      << R"( "recovery": )" << pubJson(utl::mlDsaGenerate().value()) << ","
      << R"( "heartbeatSlots": 500})"; // one override; everything else is the old network's
  // An import the chain refuses, past the server's own check of the file: an account changed (the
  // old state root no longer matches), or two accounts swapped (imports go by ascending id).
  auto refusedImport = [&](StateSnapshot tampered, const std::string &why) {
    Beacon::InitConfig bad;
    bad.workDir = (root_ / ("refused-" + std::to_string(std::hash<std::string>{}(why)))).string();
    bad.chain = tampered.config;
    bad.chain.networkId = "test-net-3";
    bad.key.genesis = {{admins[0].publicKey, admins[1].publicKey, admins[2].publicKey}, 2};
    for (auto *keys : {&bad.key.reserve, &bad.key.registrar, &bad.key.fee, &bad.key.recycle}) {
      *keys = {{newReserve.publicKey}, 1};
    }
    bad.key.genesisSigners = {admins[0].privateKey, admins[1].privateKey};
    bad.authority.recovery = {{utl::mlDsaGenerate().value().publicKey}, 1, 1};
    bad.authority.beacon.generation = 1;
    bad.authority.beacon.publicKey = utl::mlDsaGenerate().value().publicKey;
    bad.import = std::move(tampered);
    Beacon beacon;
    auto init = beacon.init(bad);
    ASSERT_FALSE(init.isOk()) << why;
    EXPECT_NE(init.error().message.find(why), std::string::npos) << init.error().message;
  };
  StateSnapshot richer = snapshot.value();
  richer.accounts.back().wallet.mBalances[AccountBuffer::ID_GENESIS] += 1;
  refusedImport(richer, "do not hash to the state root");
  StateSnapshot swapped = snapshot.value();
  std::swap(swapped.accounts[1], swapped.accounts[2]);
  refusedImport(swapped, "ascending account id");
  // A made-up state with a root of its own, under the real block's hash (B037): the header binds the
  // root to the block, so the made-up root is refused.
  StateSnapshot madeUp = richer;
  madeUp.predecessor.stateRoot = madeUp.accountsRoot();
  refusedImport(madeUp, "header is not block");

  {
    BeaconServer server;
    auto init = server.init(next.string(), adminKeyFiles, false, snapshotFile.string());
    ASSERT_TRUE(init.isOk()) << init.error().message;
  }
  EXPECT_TRUE(fs::exists(next / "data" / "signed-tip.dat")) << "the tip at the last import block is signed";

  Beacon beacon2;
  Beacon::MountConfig mount2;
  mount2.workDir = (next / "data").string();
  auto mounted = beacon2.mount(mount2); // replays the import, checking the old state root
  ASSERT_TRUE(mounted.isOk()) << mounted.error().message;
  EXPECT_EQ(beacon2.getNetworkId(), "test-net-2");
  const uint64_t lastImport = beacon2.getNextBlockId() - 1;
  ASSERT_GE(lastImport, 1u);
  for (const auto &[id, old] : before) {
    auto now = beacon2.getAccount(id);
    ASSERT_TRUE(now.isOk()) << "account " << id;
    EXPECT_EQ(now.value().wallet.mBalances, old.wallet.mBalances) << "account " << id;
    if (id == user || id == kMinerId) {
      EXPECT_EQ(now.value().wallet.publicKeys, old.wallet.publicKeys) << "account " << id << " keeps its keys";
    }
  }
  EXPECT_EQ(beacon2.getAccount(user).value().meta, before[user].meta) << "the profile, in full";
  EXPECT_EQ(beacon2.getAccount(AccountBuffer::ID_RESERVE).value().wallet.publicKeys,
            std::vector<std::string>{newReserve.publicKey}) << "system accounts take the new keys";
  EXPECT_EQ(beacon2.getAccount(AccountBuffer::ID_GENESIS).value().wallet.publicKeys.size(), 3u);
  EXPECT_EQ(beacon2.getAccount(AccountBuffer::ID_GENESIS).value().wallet.publicKeys.front(), admins[0].publicKey);
  const auto miner = beacon2.getAccount(kMinerId).value().wallet.miner;
  EXPECT_TRUE(miner.registered);
  EXPECT_EQ(miner.lastProducedSlot, 0u) << "slot counters of the old network are reset";
  auto genesis2 = beacon2.readBlock(0);
  ASSERT_TRUE(genesis2.isOk());

  // A node that ran the old network vouches for it from its own ledger (B037).
  EXPECT_TRUE(predecessor_check::check(genesis2.value(), (root_ / "beacon").string()).isOk());
  auto expectRefused = [](const pp::Roe<void> &checked, const std::string &why) {
    ASSERT_FALSE(checked.isOk()) << why;
    EXPECT_NE(checked.error().message.find(why), std::string::npos) << checked.error().message;
  };
  Ledger::ChainNode oldGenesis;
  {
    Ledger old;
    ASSERT_TRUE(old.mount((root_ / "beacon" / "data" / "ledger").string()).isOk());
    oldGenesis = old.readBlock(0).value();
  }
  expectRefused(predecessor_check::check(oldGenesis, (root_ / "beacon").string()), "imports no predecessor");
  expectRefused(predecessor_check::check(genesis2.value(), next.string()), "is not of network 'test-net'");
  {
    const fs::path shorter = root_ / "beacon-before-H";
    fs::copy(root_ / "beacon", shorter, fs::copy_options::recursive);
    Ledger cut;
    ASSERT_TRUE(cut.mount((shorter / "data" / "ledger").string()).isOk());
    ASSERT_TRUE(cut.rewindTo(block.block.index).isOk()); // ends below H
  }
  expectRefused(predecessor_check::check(genesis2.value(), (root_ / "beacon-before-H").string()),
                "does not hold block");
  {
    // An old ledger of the same network holding another block H: not the one named.
    const fs::path other = root_ / "beacon-other-H";
    fs::copy(root_ / "beacon", other, fs::copy_options::recursive);
    Ledger forked;
    ASSERT_TRUE(forked.mount((other / "data" / "ledger").string()).isOk());
    ASSERT_TRUE(forked.rewindTo(block.block.index).isOk());
    Ledger::ChainNode another = block;
    another.block.timestamp += 1;
    another.hash = utl::sha256Raw(another.block.headerToString());
    ASSERT_TRUE(forked.addBlock(another).isOk());
    ASSERT_TRUE(forked.commit().isOk());
  }
  expectRefused(predecessor_check::check(genesis2.value(), (root_ / "beacon-other-H").string()),
                "is not the one the re-genesis starts from");

  // A miner of the old network, with its old key, follows the new one and produces.
  miner_ = std::make_unique<Miner>();
  Miner::InitConfig config;
  config.workDir = (root_ / "miner2").string();
  config.minerId = kMinerId;
  config.privateKeys.push_back(minerKey_.privateKey);
  ASSERT_TRUE(miner_->init(config).isOk());
  for (uint64_t id = 0; id <= lastImport; ++id) {
    auto added = miner_->addBlock(beacon2.readBlock(id).value());
    ASSERT_TRUE(added.isOk()) << "block " << id << ": " << added.error().message;
  }
  miner_->refresh();
  auto leads = waitForLeaderSlotAfter(0);
  ASSERT_TRUE(leads.has_value());
  Ledger::TxDefault transfer; // signed by the new reserve key, for the new network
  transfer.tokenId = AccountBuffer::ID_GENESIS;
  transfer.fromWalletId = AccountBuffer::ID_RESERVE;
  transfer.toWalletId = user;
  transfer.amount = 10;
  transfer.fee = 100000;
  transfer.idempotentId = 1;
  transfer.validationTsMin = miner_->getSlotStartTime(*leads);
  transfer.validationTsMax = transfer.validationTsMin + 60;
  Ledger::Record rec;
  rec.type = Ledger::T_DEFAULT;
  rec.data = utl::binaryPack(transfer);
  Ledger::Record oldNetwork = rec;
  oldNetwork.signatures = {utl::mlDsaSign(newReserve.privateKey, rec.signingMessage("test-net")).value()};
  EXPECT_FALSE(miner_->addTransaction(oldNetwork).isOk()) << "a signature for the old network is no good here";
  rec.signatures = {utl::mlDsaSign(newReserve.privateKey, rec.signingMessage("test-net-2")).value()};
  auto pooled = miner_->addTransaction(rec);
  ASSERT_TRUE(pooled.isOk()) << pooled.error().message;
  Ledger::ChainNode produced;
  ASSERT_TRUE(produceWhenSealTime(produced));
  EXPECT_EQ(produced.block.index, lastImport + 1);
  auto committed = beacon2.addBlock(produced);
  EXPECT_TRUE(committed.isOk()) << committed.error().message;
}
