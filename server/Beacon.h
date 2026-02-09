#ifndef PP_LEDGER_BEACON_H
#define PP_LEDGER_BEACON_H

#include "Validator.h"
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
 * Beacon - Core consensus and ledger management
 * 
 * Responsibilities:
 * - Maintain full blockchain history from genesis
 * - Manage Ouroboros consensus protocol
 * - Determine checkpoint locations for data pruning
 * - Verify blocks (but does not produce them)
 * - Serve as authoritative data source for the network
 * - Coordinate with BeaconServer for network communication
 * 
 * Design:
 * - Beacons are limited in number and act as data backups
 * - They maintain checkpoints to allow pruning of old block data
 * - Checkpoints are created when data exceeds 1GB and is older than 1 year
 * - Miners produce blocks, Beacons verify and archive them
 */
class Beacon : public Validator {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  
  template <typename T> using Roe = ResultOrError<T, Error>;

  struct InitKeyConfig {
    std::vector<std::string> genesis;
    std::vector<std::string> fee;
    std::vector<std::string> reserve;

    nlohmann::json toJson() const;
  };

  struct InitConfig {
    // Base configuration
    std::string workDir;
    BlockChainConfig chain;
    InitKeyConfig key;
  };

  struct MountConfig {
    std::string workDir;
  };

  Beacon();
  ~Beacon() override = default;

  // ----------------- accessors -------------------------------------
  bool needsCheckpoint() const;

  Roe<uint64_t> getSlotLeader(uint64_t slot) const;
  uint64_t getLastCheckpointId() const;
  uint64_t getCurrentCheckpointId() const;
  Roe<std::vector<uint64_t>> getCheckpoints() const;

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig& config);
  Roe<void> mount(const MountConfig& config);
  void refresh();

  Roe<void> addBlock(const Ledger::ChainNode& block);

private:
  struct Config {
    std::string workDir;
    BlockChainConfig chain;
  };

  Roe<void> validateBlock(const Ledger::ChainNode& block) const;
  Roe<void> evaluateCheckpoints();
  uint64_t getBlockAge(uint64_t blockId) const;
  Roe<void> createCheckpoint(uint64_t blockId);
  Ledger::ChainNode createGenesisBlock(const BlockChainConfig& config, const InitKeyConfig& key) const;

  Config config_;
  uint64_t currentCheckpointId_{ 0 };
  uint64_t lastCheckpointId_{ 0 };
};

// Ostream operators for easy logging
std::ostream& operator<<(std::ostream& os, const Beacon::CheckpointConfig& config);
std::ostream& operator<<(std::ostream& os, const Beacon::InitConfig& config);
std::ostream& operator<<(std::ostream& os, const Beacon::MountConfig& config);
} // namespace pp

#endif // PP_LEDGER_BEACON_H