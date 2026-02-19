#ifndef PP_LEDGER_MINER_H
#define PP_LEDGER_MINER_H

#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../network/Types.hpp"
#include "Chain.h"

#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace pp {

/**
 * Miner - Block Producer
 *
 * Responsibilities:
 * - Produce blocks when selected as slot leader
 * - Maintain local blockchain and ledger state
 * - Process transactions and include them in blocks
 * - Sync with network to get latest blocks
 * - Reinitialize from checkpoints when needed
 * - Validate incoming blocks from other miners
 *
 * Design:
 * - Miners are the primary block producers in the network
 * - Multiple miners compete to produce blocks based on stake
 * - Can sync from checkpoints to reduce initial sync time
 * - Maintains transaction pool for pending transactions
 * - Uses Ouroboros consensus for slot leader selection
 */
class Miner : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  struct InitConfig {
    std::string workDir;
    int64_t timeOffset{0};
    uint64_t minerId{0};
    uint64_t startingBlockId{0};
    uint64_t checkpointId{0};
    std::vector<std::string> privateKeys; // hex-encoded private keys (multiple signatures)
  };

  Miner();
  ~Miner() override = default;

  // ----------------- accessors -------------------------------------
  bool isSlotLeader() const;
  Roe<uint64_t> getSlotLeaderId() const;

  uint64_t getStake() const;
  size_t getPendingTransactionCount() const;
  uint64_t getNextBlockId() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Ledger::ChainNode> getBlock(uint64_t blockId) const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;
  std::string calculateHash(const Ledger::Block &block) const;

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig &config);
  void refresh();

  Roe<void>
  addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx);
  Roe<void> addBlock(const Ledger::ChainNode &block);

  /** Cache a transaction for forwarding retry when slot leader address is unknown. */
  void addToForwardCache(const Ledger::SignedData<Ledger::Transaction> &signedTx);
  /** Take all cached transactions for retry; returns and clears the cache. */
  std::vector<Ledger::SignedData<Ledger::Transaction>> drainForwardCache();

  Roe<bool> produceBlock(Ledger::ChainNode &block);
  void markBlockProduction(const Ledger::ChainNode &block);

private:
  constexpr static const char *DIR_LEDGER = "ledger";

  struct Config {
    std::string workDir;
    uint64_t minerId{0};
    uint64_t tokenId{AccountBuffer::ID_GENESIS};
    std::vector<std::string> privateKeys; // hex-encoded (multiple signatures)
    uint64_t checkpointId{0};
  };

  struct SlotCache {
    uint64_t slot{0};
    bool isLeader{false};
    std::vector<Ledger::SignedData<Ledger::Transaction>> txRenewals;
  };

  /** Transactions for the next block and count of pending included (for trimmed). */
  struct BlockTxSet {
    std::vector<Ledger::SignedData<Ledger::Transaction>> signedTxes;
    size_t nPendingIncluded{0};
  };

  BlockTxSet getBlockTransactionSet() const;

  Roe<void> initSlotCache(uint64_t slot);
  Roe<Ledger::ChainNode> createBlock(
      uint64_t slot,
      const std::vector<Ledger::SignedData<Ledger::Transaction>> &signedTxes);

  Chain chain_;
  Config config_;
  AccountBuffer bufferBank_;
  std::vector<Ledger::SignedData<Ledger::Transaction>> pendingTxes_;
  std::vector<Ledger::SignedData<Ledger::Transaction>> forwardCache_;

  uint64_t lastProducedBlockId_{0};
  // slot last produced block in (at most one per slot)
  uint64_t lastProducedSlot_{0};
  SlotCache slotCache_; // Cache data for block production
};

} // namespace pp

#endif // PP_LEDGER_MINER_H} // namespace pp
