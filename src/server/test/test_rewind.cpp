#include "Rewind.h"
#include "../../ledger/Ledger.h"
#include "TestTempPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace pp;

// beacon-authority bp1 (B022): a rewind is an order written before a restart and applied at the
// next start, before the ledger mounts.

namespace {

class RewindTest : public ::testing::Test {
protected:
  void SetUp() override {
    dataDir_ = pp::test::uniqueTempPath("pp-ledger-rewind-test");
    std::error_code ec;
    std::filesystem::remove_all(dataDir_, ec);
    std::filesystem::create_directories(dataDir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dataDir_, ec);
  }

  std::string ledgerDir() const { return (dataDir_ / "ledger").string(); }

  /** A ledger holding blocks `from` .. `to`, closed. */
  void makeLedger(uint64_t from, uint64_t to) {
    Ledger ledger;
    Ledger::InitConfig config;
    config.workDir = ledgerDir();
    config.startingBlockId = from;
    ASSERT_TRUE(ledger.init(config).isOk());
    for (uint64_t i = from; i <= to; ++i) {
      Ledger::ChainNode block;
      block.block.index = i;
      block.hash = "hash-" + std::to_string(i);
      ASSERT_TRUE(ledger.addBlock(block).isOk());
    }
  }

  uint64_t nextBlockId() const {
    Ledger ledger;
    EXPECT_TRUE(ledger.mount(ledgerDir()).isOk());
    return ledger.getNextBlockId();
  }

  static Client::RewindReport order(uint64_t keptUpTo, uint64_t droppedUpTo, uint64_t generation = 3) {
    Client::RewindReport report;
    report.keptUpTo = keptUpTo;
    report.droppedUpTo = droppedUpTo;
    report.generation = generation;
    return report;
  }

  std::filesystem::path dataDir_;
};

} // namespace

TEST_F(RewindTest, NothingPendingChangesNothing) {
  makeLedger(0, 4);
  auto applied = rewind::applyPendingOrder(dataDir_.string(), ledgerDir());
  ASSERT_TRUE(applied.isOk()) << applied.error().message;
  EXPECT_FALSE(applied.value().has_value());
  EXPECT_FALSE(rewind::lastApplied(dataDir_.string()).value().has_value());
  EXPECT_EQ(nextBlockId(), 5u);
}

// The order cuts the ledger after the cutover and is then kept as the last rewind; applying again
// (a crash before the order was removed) changes nothing.
TEST_F(RewindTest, AnOrderCutsTheLedgerOnceAndIsKept) {
  makeLedger(0, 6);
  ASSERT_TRUE(rewind::writeOrder(dataDir_.string(), order(2, 6)).isOk());
  auto applied = rewind::applyPendingOrder(dataDir_.string(), ledgerDir());
  ASSERT_TRUE(applied.isOk()) << applied.error().message;
  ASSERT_TRUE(applied.value().has_value());
  EXPECT_EQ(*applied.value(), order(2, 6));
  EXPECT_EQ(nextBlockId(), 3u);
  EXPECT_FALSE(std::filesystem::exists(dataDir_ / rewind::FILE_ORDER));
  EXPECT_EQ(rewind::lastApplied(dataDir_.string()).value(), std::optional<Client::RewindReport>(order(2, 6)));

  ASSERT_TRUE(rewind::writeOrder(dataDir_.string(), order(2, 6)).isOk()); // as if the removal was lost
  ASSERT_TRUE(rewind::applyPendingOrder(dataDir_.string(), ledgerDir()).isOk());
  EXPECT_EQ(nextBlockId(), 3u);
}

// A cutover below the ledger's first block (a miner started at a later checkpoint): the ledger goes
// and the node joins fresh.
TEST_F(RewindTest, ACutoverBelowTheFirstBlockJoinsFresh) {
  makeLedger(10, 14);
  ASSERT_TRUE(rewind::writeOrder(dataDir_.string(), order(5, 14)).isOk());
  ASSERT_TRUE(rewind::applyPendingOrder(dataDir_.string(), ledgerDir()).isOk());
  EXPECT_FALSE(std::filesystem::exists(ledgerDir()));
  EXPECT_TRUE(rewind::lastApplied(dataDir_.string()).value().has_value());
}

TEST_F(RewindTest, AnUnreadableOrderStopsTheStart) {
  makeLedger(0, 3);
  std::ofstream(dataDir_ / rewind::FILE_ORDER) << "garbage";
  EXPECT_FALSE(rewind::applyPendingOrder(dataDir_.string(), ledgerDir()).isOk());
  EXPECT_EQ(nextBlockId(), 4u) << "the ledger is left as it was";
}
