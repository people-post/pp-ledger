// load-resilience lr8: the idempotency scan from IdempotencyScanCache gives exactly the answers of the
// scan that reads every block — duplicates, misses, key errors, read failures — before and after a
// rewind replaces blocks.
#include "../ErrorCodes.h"
#include "../TxIdempotency.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

using namespace pp;

namespace {

constexpr int64_t kGenesis = 1'700'000'000;
constexpr uint64_t kSlotSeconds = 2;

/** A record's key is in its data: "wallet:id"; "-" has none; "!" cannot be worked out. */
chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>> keyOf(const Ledger::Record &record) {
  if (record.data == "!") {
    return chain_tx::TxError(1, "undecodable record");
  }
  if (record.data == "-") {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  const auto colon = record.data.find(':');
  return std::optional<std::pair<uint64_t, uint64_t>>{
      std::make_pair(std::stoull(record.data.substr(0, colon)), std::stoull(record.data.substr(colon + 1)))};
}

class IdempotencyScanTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp-idem-" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir_);
    Ledger::InitConfig config;
    config.workDir = dir_.string();
    ASSERT_TRUE(ledger_.init(config).isOk());
    consensus_.setSlotTiming(kGenesis, kSlotSeconds, 10);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /** Append a block for `slot` (sealed as its slot ends) holding `records`. */
  void add(uint64_t slot, std::vector<std::string> records) {
    Ledger::ChainNode node;
    node.block.index = ledger_.getNextBlockId();
    node.block.slot = slot;
    node.block.timestamp = consensus_.getSlotStartTime(slot) + static_cast<int64_t>(kSlotSeconds);
    for (auto &data : records) {
      Ledger::Record record;
      record.data = std::move(data);
      node.block.records.push_back(std::move(record));
    }
    node.hash = "h" + std::to_string(node.block.index) + "-" + std::to_string(slot);
    ASSERT_TRUE(ledger_.addBlock(node).isOk());
  }

  /** Both scans over every query in a grid; returns how many found a duplicate. */
  size_t expectSameAnswers(chain_tx::IdempotencyScanCache &cache, uint64_t maxSlot) {
    size_t duplicates = 0;
    for (uint64_t wallet = 1; wallet <= 3; ++wallet) {
      for (uint64_t id = 1; id <= 6; ++id) {
        for (uint64_t slotMin = 0; slotMin <= maxSlot + 1; ++slotMin) {
          for (uint64_t slotMax = slotMin; slotMax <= maxSlot + 2; slotMax += 3) {
            auto plain = chain_tx::checkIdempotency(ledger_, consensus_, id, wallet, slotMin, slotMax, keyOf);
            auto cached = chain_tx::checkIdempotency(ledger_, consensus_, id, wallet, slotMin, slotMax, keyOf, &cache);
            EXPECT_EQ(plain.isOk(), cached.isOk()) << wallet << ":" << id << " in " << slotMin << ".." << slotMax;
            if (!plain.isOk() && !cached.isOk()) {
              EXPECT_EQ(plain.error().code, cached.error().code);
              EXPECT_EQ(plain.error().message, cached.error().message);
              duplicates += plain.error().code == chain_err::E_TX_IDEMPOTENCY ? 1 : 0;
            }
          }
        }
      }
    }
    return duplicates;
  }

  std::filesystem::path dir_;
  Ledger ledger_;
  consensus::SlotCommittee consensus_;
};

TEST_F(IdempotencyScanTest, CachedScanGivesTheSameAnswers) {
  chain_tx::IdempotencyScanCache cache;
  EXPECT_EQ(expectSameAnswers(cache, 0), 0u) << "no block yet";
  std::mt19937 rng(7);
  uint64_t slot = 0;
  for (int i = 0; i < 24; ++i) {
    slot += 1 + rng() % 3; // empty slots between some blocks
    std::vector<std::string> records;
    for (int r = 0; r < 4; ++r) {
      const auto pick = rng() % 10;
      records.push_back(pick == 0 ? "-" : std::to_string(1 + rng() % 3) + ":" + std::to_string(1 + rng() % 6));
    }
    if (i == 17) {
      records.insert(records.begin() + 2, "!"); // a key error mid-block: what comes before it still counts
    }
    add(slot, records);
  }
  EXPECT_GT(expectSameAnswers(cache, slot), 0u) << "the grid finds duplicates";

  // A rewind replaces blocks under the cache: it must not answer from the old ones.
  ASSERT_TRUE(ledger_.rewindTo(10).isOk());
  uint64_t again = consensus_.getSlotFromTimestamp(ledger_.readLastBlock().value().block.timestamp);
  for (int i = 0; i < 6; ++i) {
    again += 1;
    add(again, {"9:9", std::to_string(1 + i % 3) + ":" + std::to_string(1 + i)});
  }
  expectSameAnswers(cache, again);
}

} // namespace
