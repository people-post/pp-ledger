#ifndef PP_LEDGER_RELAY_H
#define PP_LEDGER_RELAY_H

#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../network/Types.hpp"
#include "Chain.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <vector>

namespace pp {

/**
 * Relay - Core consensus and ledger management
 *
 * Responsibilities:
 * - Maintain full blockchain history from genesis
 * - Serve as authoritative data source for the network
 * - Coordinate with RelayServer for network communication
 */
class Relay : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  struct InitConfig {
    std::string workDir;
    int64_t timeOffset{0};
    uint64_t startingBlockId{0};
  };

  Relay();
  ~Relay() override = default;

  // ----------------- accessors -------------------------------------
  uint64_t getLastCheckpointId() const;
  uint64_t getCurrentCheckpointId() const;
  uint64_t getNextBlockId() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  /** Slot duration in seconds (for sync rate limiting). */
  uint64_t getSlotDuration() const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;

  Roe<Ledger::ChainNode> readBlock(uint64_t blockId) const;
  std::string calculateHash(const Ledger::Block &block) const;
  /** Find transactions involving walletId, scanning backwards from ioBlockId (0 = latest). ioBlockId is updated to the last block scanned. */
  Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>>
  findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const;

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig &config);
  void refresh();

  Roe<void> addBlock(const Ledger::ChainNode &block);

private:
  constexpr static const char *DIR_LEDGER = "ledger";

  struct Config {
    std::string workDir;
    int64_t timeOffset{0};
  };

  Chain chain_;
  Config config_;
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Relay::InitConfig &config);

} // namespace pp

#endif // PP_LEDGER_RELAY_H
