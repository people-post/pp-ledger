// Ledger scale (projects/load-resilience lr9, the data axis): a chain of real signed transfers built
// in-process on virtual time, then what grows with it — a follower validating it block by block
// (sync from genesis), a node restarting on it (Chain::loadFromLedger replays it), and wallet
// history requests on it. Skipped unless PP_LEDGER_SCALE=1 (--suite scale); sizes from
//   PP_LEDGER_SCALE_ACCOUNTS (1000), PP_LEDGER_SCALE_BLOCKS (1000), PP_LEDGER_SCALE_TXS (100 a block),
// and the report also written as JSON to PP_LEDGER_SCALE_JSON when set.
#include "ChainFixtures.h"

#include "common/io/Json.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <thread>

using namespace pp;
using namespace pp::chain_test;

namespace {

using Clock = std::chrono::steady_clock;

/** An environment variable, if set and not empty (MSVC deprecates getenv, C4996: as env_config::get). */
std::optional<std::string> env(const char *name) {
#ifdef _WIN32
  char *buffer = nullptr;
  size_t length = 0;
  if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
    return std::nullopt;
  }
  std::string value(buffer);
  std::free(buffer);
#else
  const char *raw = std::getenv(name);
  std::string value = raw ? raw : "";
#endif
  return value.empty() ? std::nullopt : std::optional<std::string>(value);
}

uint64_t envOr(const char *name, uint64_t fallback) {
  const auto value = env(name);
  return value ? std::strtoull(value->c_str(), nullptr, 10) : fallback;
}

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

/** `count` items made by `make(i)`, in parallel over the machine's cores (ML-DSA signing dominates). */
template <typename T, typename Make> std::vector<T> parallelMake(size_t count, Make make) {
  std::vector<T> out(count);
  const size_t workers = std::max<size_t>(1, std::thread::hardware_concurrency());
  std::vector<std::future<void>> running;
  for (size_t w = 0; w < workers; ++w) {
    running.push_back(std::async(std::launch::async, [&, w]() {
      for (size_t i = w; i < count; i += workers) {
        out[i] = make(i);
      }
    }));
  }
  for (auto &r : running) {
    r.get();
  }
  return out;
}

struct UserKey {
  uint64_t id{0};
  utl::MlDsaKeyPair keys;
};

class LedgerScaleTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!env("PP_LEDGER_SCALE")) {
      GTEST_SKIP() << "ledger scale runs with PP_LEDGER_SCALE=1 (--suite scale)";
    }
    accounts_ = envOr("PP_LEDGER_SCALE_ACCOUNTS", 1000);
    blocks_ = envOr("PP_LEDGER_SCALE_BLOCKS", 1000);
    txs_ = envOr("PP_LEDGER_SCALE_TXS", 100);
    harness_.SetUp([this](Chain::BlockChainConfig &config) {
      config.maxTransactionsPerBlock = txs_;
      config.checkpoint.minBlocks = 1'000'000'000; // no renewals here: their echo is measured apart
      config.maxIssuancePerEpoch = 0;
    });
  }
  void TearDown() override { harness_.TearDown(); }

  /** A transfer window around the block's time, as clients sign them (60 s back, an hour ahead). */
  template <typename Tx> void window(Tx &tx, uint64_t slot, uint64_t idempotentId) {
    const int64_t at = harness_.producer.getSlotStartTime(slot);
    tx.idempotentId = idempotentId;
    tx.validationTsMin = at - 60;
    tx.validationTsMax = at + 3600;
  }

  /** Seal and commit a block of `records` at `slot` on the producer; returns the seconds it took. */
  double produce(uint64_t slot, const std::vector<Ledger::Record> &records) {
    const auto start = Clock::now();
    tip_ = makeNextBlockAtSlot(harness_.producer, tip_, slot, records);
    auto added = harness_.producer.addBlock(tip_);
    EXPECT_TRUE(added.isOk()) << "block at slot " << slot << ": " << (added ? "" : added.error().message);
    return secondsSince(start);
  }

  ComposeHarness harness_;
  Ledger::ChainNode tip_;
  uint64_t accounts_{0};
  uint64_t blocks_{0};
  uint64_t txs_{0};
};

TEST_F(LedgerScaleTest, SyncRestartAndRequestsOnALongChain) {
  auto &producer = harness_.producer;
  producer.setLedgerCommitPolicy(Ledger::CommitPolicy::everyBlock()); // as the beacon: durable at once
  tip_ = harness_.genesis;
  uint64_t slot = tip_.block.slot;
  uint64_t idempotent = 1;
  std::mt19937_64 rng(42);
  pp::common::Object report;

  // Accounts, funded from the reserve, a block's worth at a time.
  auto t0 = Clock::now();
  auto users = parallelMake<UserKey>(accounts_, [](size_t i) {
    return UserKey{(1ULL << 30) + 1000 + i, makeKeyPair()}; // below 2^30 only the registrar issues ids
  });
  // Half the supply shared out; an account spends ~11 a transfer it sends, ~blocks x txs / accounts of them.
  const uint64_t perAccount = (AccountBuffer::INITIAL_TOKEN_SUPPLY / 2) / accounts_;
  ASSERT_GT(perAccount, 20 * 11 * (blocks_ * txs_ / accounts_ + 1)) << "too few tokens for these sizes";
  for (size_t first = 0; first < users.size(); first += txs_) {
    ++slot;
    std::vector<Ledger::Record> records;
    for (size_t i = first; i < std::min<size_t>(users.size(), first + txs_); ++i) {
      Ledger::TxNewUser tx;
      tx.fromWalletId = AccountBuffer::ID_RESERVE;
      tx.toWalletId = users[i].id;
      tx.amount = perAccount;
      tx.fee = 10'000;
      tx.meta = makeUserAccount(users[i].keys.publicKey, static_cast<int64_t>(tx.amount)).ltsToString();
      window(tx, slot, idempotent++);
      records.push_back(makeRecord(Ledger::T_NEW_USER, tx, harness_.reserveKey, producer.getNetworkId()));
    }
    produce(slot, records);
    if (HasFailure()) {
      return;
    }
  }
  std::cout << "accounts: " << users.size() << " in " << std::fixed << std::setprecision(1) << secondsSince(t0)
            << " s\n";

  // Blocks of transfers between random accounts, signed in parallel, sealed and committed in turn.
  t0 = Clock::now();
  struct Planned {
    uint64_t from;
    uint64_t to;
    uint64_t idempotent;
    uint64_t slot;
  };
  std::vector<Planned> plan;
  plan.reserve(blocks_ * txs_);
  for (uint64_t b = 0; b < blocks_; ++b) {
    const uint64_t blockSlot = slot + 1 + b;
    for (uint64_t t = 0; t < txs_; ++t) {
      const uint64_t from = rng() % users.size();
      uint64_t to = rng() % users.size();
      if (to == from) {
        to = (to + 1) % users.size();
      }
      plan.push_back(Planned{from, to, idempotent++, blockSlot});
    }
  }
  auto signedTxs = parallelMake<Ledger::Record>(plan.size(), [&](size_t i) {
    Ledger::TxDefault tx;
    tx.tokenId = AccountBuffer::ID_GENESIS;
    tx.fromWalletId = users[plan[i].from].id;
    tx.toWalletId = users[plan[i].to].id;
    tx.amount = 1;
    tx.fee = 10;
    window(tx, plan[i].slot, plan[i].idempotent);
    return makeRecord(Ledger::T_DEFAULT, tx, users[plan[i].from].keys, producer.getNetworkId());
  });
  const double signSeconds = secondsSince(t0);
  std::vector<double> sealSeconds;
  for (uint64_t b = 0; b < blocks_; ++b) {
    ++slot;
    std::vector<Ledger::Record> records(signedTxs.begin() + static_cast<std::ptrdiff_t>(b * txs_),
                                        signedTxs.begin() + static_cast<std::ptrdiff_t>((b + 1) * txs_));
    sealSeconds.push_back(produce(slot, records));
    if (HasFailure()) {
      return;
    }
  }
  std::sort(sealSeconds.begin(), sealSeconds.end());
  const uint64_t chainBlocks = producer.getNextBlockId();
  const uint64_t chainTxs = accounts_ + blocks_ * txs_;
  std::cout << "chain: " << chainBlocks << " blocks, " << chainTxs << " transactions (signing " << signSeconds
            << " s); a leader's block of " << txs_ << ": p50 " << sealSeconds[sealSeconds.size() / 2] * 1000
            << " ms, max " << sealSeconds.back() * 1000 << " ms\n";
  report.setJsonUInt("blocks", chainBlocks);
  report.setJsonUInt("transactions", chainTxs);
  report.setJsonUInt("transactionsPerBlock", txs_);
  report.set("sealMsP50", sealSeconds[sealSeconds.size() / 2] * 1000);
  report.set("sealMsMax", sealSeconds.back() * 1000);

  // Sync from genesis: the peer validates every block in full, as a follower does.
  auto &peer = harness_.peer;
  t0 = Clock::now();
  for (uint64_t id = 1; id < chainBlocks; ++id) {
    auto block = producer.readBlock(id);
    ASSERT_TRUE(block.isOk());
    auto added = peer.addBlock(block.value());
    ASSERT_TRUE(added.isOk()) << "peer, block " << id << ": " << added.error().message;
  }
  const double syncSeconds = secondsSince(t0);
  const double syncBlocksPerSecond = static_cast<double>(chainBlocks - 1) / syncSeconds;
  const double syncTxsPerSecond = static_cast<double>(chainTxs) / syncSeconds;
  std::cout << "sync from genesis: " << syncSeconds << " s — " << syncBlocksPerSecond << " blocks/s, "
            << syncTxsPerSecond << " tx/s\n";
  report.set("syncSeconds", syncSeconds);
  report.set("syncTxsPerSecond", syncTxsPerSecond);

  // Restart: a node mounting this ledger replays it from genesis (Beacon::mount, loadFromLedger(0)).
  Chain restarted;
  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  restarted.initConsensus(consensusConfig);
  ASSERT_TRUE(restarted.mountLedger(harness_.producerDir.string()).isOk());
  t0 = Clock::now();
  auto loaded = restarted.loadFromLedger(0);
  const double replaySeconds = secondsSince(t0);
  ASSERT_TRUE(loaded.isOk()) << loaded.error().message;
  ASSERT_EQ(loaded.value(), chainBlocks);
  const double replayTxsPerSecond = static_cast<double>(chainTxs) / replaySeconds;
  std::cout << "restart (replay from genesis): " << replaySeconds << " s — " << replayTxsPerSecond << " tx/s\n";
  report.set("replaySeconds", replaySeconds);
  report.set("replayTxsPerSecond", replayTxsPerSecond);

  // Wallet history from the tip: one request scans at most MAX_BLOCKS_TO_SCAN_FOR_WALLET_TX blocks.
  std::vector<double> walletMs;
  for (size_t i = 0; i < 50; ++i) {
    uint64_t from = 0;
    const auto start = Clock::now();
    auto found = producer.findTransactionsByWalletId(users[i % users.size()].id, from);
    walletMs.push_back(secondsSince(start) * 1000);
    ASSERT_TRUE(found.isOk());
  }
  std::sort(walletMs.begin(), walletMs.end());
  std::cout << "wallet history from the tip: p50 " << walletMs[walletMs.size() / 2] << " ms, max " << walletMs.back()
            << " ms a request\n";
  report.set("walletMsP50", walletMs[walletMs.size() / 2]);

  // Checks: a restart replays about as fast as a follower syncs (it was quadratic, lr9), and a wallet
  // history request stays cheap whatever the chain's length (it scans a bounded number of blocks).
  EXPECT_GE(replayTxsPerSecond, syncTxsPerSecond / 2) << "a restart replays much slower than a sync";
  EXPECT_LT(walletMs[walletMs.size() / 2], 50.0) << "wallet history slow on a long chain";

  // What it means for a network: tx/s a node replays or syncs, and the chain it gets through in
  // startupSyncTimeout (5 minutes).
  const double startupSyncSeconds = 300;
  std::cout << "in 5 minutes a node validates " << static_cast<uint64_t>(syncTxsPerSecond * startupSyncSeconds)
            << " transactions syncing, replays " << static_cast<uint64_t>(replayTxsPerSecond * startupSyncSeconds)
            << " restarting\n";

  if (const auto path = env("PP_LEDGER_SCALE_JSON")) {
    auto json = pp::common::io::valueToJsonString(pp::common::Value(std::make_shared<pp::common::Object>(report)), 2);
    ASSERT_TRUE(json.isOk());
    std::ofstream(*path) << json.value() << "\n";
  }
}

} // namespace
