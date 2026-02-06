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

  struct CheckpointConfig {
    uint64_t minSizeBytes{ 1024ULL * 1024 * 1024 }; // 1GB default
    uint64_t ageSeconds{ 365ULL * 24 * 3600 }; // 1 year default
  };

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
    CheckpointConfig checkpoint;
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

  Roe<void> addBlock(const Ledger::ChainNode& block);

private:
  struct Config {
    std::string workDir;
    BlockChainConfig chain;
    CheckpointConfig checkpoint;
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
inline std::ostream& operator<<(std::ostream& os, const Beacon::CheckpointConfig& config) {
  os << "CheckpointConfig{minSizeBytes=" << config.minSizeBytes 
     << " (" << (config.minSizeBytes / (1024*1024)) << " MB), "
     << "ageSeconds=" << config.ageSeconds
     << " (" << (config.ageSeconds / (24*3600)) << " days)}";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const Beacon::InitConfig& config) {
  os << "InitConfig{workDir=\"" << config.workDir << "\", "
     << "chain=" << config.chain << "}";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const Beacon::MountConfig& config) {
  os << "MountConfig{workDir=\"" << config.workDir << "\", "
     << "checkpoint=" << config.checkpoint << "}";
  return os;
}

} // namespace pp

#endif // PP_LEDGER_BEACON_H