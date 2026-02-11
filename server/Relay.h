#ifndef PP_LEDGER_RELAY_H
#define PP_LEDGER_RELAY_H

#include "Chain.h"
#include "../ledger/Ledger.h"
#include "../consensus/Ouroboros.h"
#include "../network/Types.hpp"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"

#include <string>
#include <cstdint>
#include <list>
#include <vector>
#include <mutex>

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
    int64_t timeOffset{ 0 };
    uint64_t startingBlockId{ 0 };
  };

  Relay();
  ~Relay() override = default;

  // ----------------- accessors -------------------------------------
  uint64_t getLastCheckpointId() const;
  uint64_t getCurrentCheckpointId() const;
  uint64_t getNextBlockId() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Ledger::ChainNode> getBlock(uint64_t blockId) const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;
  std::string calculateHash(const Ledger::Block& block) const;

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig& config);
  void refresh();

  Roe<void> addBlock(const Ledger::ChainNode& block);

private:
  constexpr static const char* DIR_LEDGER = "ledger";

  struct Config {
    std::string workDir;
    int64_t timeOffset{ 0 };
  };

  Chain validator_;
  Config config_;
};

// Ostream operators for easy logging
std::ostream& operator<<(std::ostream& os, const Relay::InitConfig& config);

} // namespace pp

#endif // PP_LEDGER_RELAY_H