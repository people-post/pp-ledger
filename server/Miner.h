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
    std::string privateKey; // hex-encoded private key
  };

  Miner();
  ~Miner() override = default;

  // ----------------- accessors -------------------------------------
  bool isSlotLeader() const;

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

  Roe<bool> produceBlock(Ledger::ChainNode &block);
  void markBlockProduction(const Ledger::ChainNode &block);

private:
  constexpr static const char *DIR_LEDGER = "ledger";

  struct Config {
    std::string workDir;
    uint64_t minerId{0};
    uint64_t tokenId{AccountBuffer::ID_GENESIS};
    std::string privateKey; // hex-encoded
    uint64_t checkpointId{0};
  };

  struct SlotCache {
    uint64_t slot{0};
    bool isLeader{false};
    std::vector<Ledger::SignedData<Ledger::Transaction>> txRenewals;
  };

  Roe<void> initSlotCache(uint64_t slot);
  Roe<Ledger::ChainNode> createBlock(uint64_t slot);

  Chain chain_;
  Config config_;
  AccountBuffer bufferBank_;
  std::vector<Ledger::SignedData<Ledger::Transaction>> pendingTxes_;
  uint64_t lastProducedBlockId_{0};
  uint64_t lastProducedSlot_{
      0}; // slot we last produced a block for (at most one per slot)
  SlotCache slotCache_; // Cache data for block production
};

} // namespace pp

#endif // PP_LEDGER_MINER_H} // namespace pp
