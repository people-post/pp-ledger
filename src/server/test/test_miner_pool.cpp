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
 * Fresh beacon genesis (1 s slots) and a Miner running as the reserve account.
 * After genesis the committee is {fee, reserve} with equal weight, so the
 * reserve leads about half the slots; tests wait for slots it leads.
 */
class MinerPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / "pp-ledger-miner-pool-test";
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_ / "beacon");
    std::ofstream(root_ / "beacon" / "init-config.json")
        << R"({"slotDuration": 1, "slotsPerEpoch": 1000, "maxCustomMetaSize": 10000,)"
        << R"( "maxTransactionsPerBlock": 100, "minFeeCoefficients": [1, 1, 0],)"
        << R"( "freeCustomMetaSize": 1024, "checkpointMinBlocks": 1000,)"
        << R"( "checkpointMinAgeSeconds": 0, "heartbeatSlots": 1000})";

    BeaconServer beacon;
    auto keys = beacon.init((root_ / "beacon").string());
    ASSERT_TRUE(keys.isOk()) << keys.error().message;
    reserveKeys_ = keys.value().reserve;

    Ledger ledger;
    ASSERT_TRUE(ledger.mount((root_ / "beacon" / "data" / "ledger").string()).isOk());
    auto genesis = ledger.readBlock(0);
    ASSERT_TRUE(genesis.isOk()) << genesis.error().message;

    miner_ = std::make_unique<Miner>();
    Miner::InitConfig config;
    config.workDir = (root_ / "miner").string();
    config.minerId = AccountBuffer::ID_RESERVE;
    for (const auto &k : reserveKeys_) {
      config.privateKeys.push_back(k.privateKey);
    }
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
      auto sig = utl::mlDsaSign(k.privateKey, rec.signingMessage(""));
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
