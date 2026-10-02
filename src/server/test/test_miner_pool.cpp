#include "BeaconServer.h"
#include "Miner.h"
#include "../../chain/AccountBuffer.h"
#include "../../ledger/Ledger.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Utilities.h"

#include <gtest/gtest.h>

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
    root_ = fs::temp_directory_path() / "pp-ledger-miner-pool-test";
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_ / "beacon");
    auto minerKey = utl::mlDsaGenerate();
    ASSERT_TRUE(minerKey.isOk());
    // Each system account's holder makes its own key; genesis signs genesis.
    // The genesis (admin) account is 2-of-3; two of its keys sign genesis.
    std::vector<utl::MlDsaKeyPair> genesisKeys = {utl::mlDsaGenerate().value(), utl::mlDsaGenerate().value(),
                                                  utl::mlDsaGenerate().value()};
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

  /** Wait for a slot after `after` that this miner leads; that slot, or nullopt on timeout. */
  std::optional<uint64_t> waitForLeaderSlotAfter(int64_t after) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < until) {
      miner_->refresh();
      const uint64_t slot = miner_->getCurrentSlot();
      if (static_cast<int64_t>(slot) > after && miner_->isSlotLeader()) {
        return slot;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
  }

  fs::path root_;
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

} // namespace
