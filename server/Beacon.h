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

  struct InitConfig {
    // Base configuration
    std::string workDir;
    BlockChainConfig chain;
  };

  struct MountConfig {
    std::string workDir;
    CheckpointConfig checkpoint;
  };

  struct Stakeholder {
    uint64_t id;
    network::TcpEndpoint endpoint;
    uint64_t stake;
  };

  Beacon();
  ~Beacon() override = default;


  bool needsCheckpoint() const;
  Roe<bool> shouldAcceptChain(const Validator::BlockChain& candidateChain) const;

  Roe<uint64_t> getSlotLeader(uint64_t slot) const;
  uint32_t getVersion() const { return VERSION; }
  uint64_t getCurrentCheckpointId() const;
  uint64_t getTotalStake() const;
  const std::list<Stakeholder>& getStakeholders() const;
  Roe<std::vector<uint64_t>> getCheckpoints() const;

  // Initialization
  Roe<void> init(const InitConfig& config);
  Roe<void> mount(const MountConfig& config);

  void addStakeholder(const Stakeholder& stakeholder);
  void removeStakeholder(uint64_t stakeholderId);
  void updateStake(uint64_t stakeholderId, uint64_t newStake);

  Roe<void> addBlock(const Ledger::ChainNode& block);
  Roe<void> validateBlock(const Ledger::ChainNode& block) const;
  Roe<void> syncChain(const Validator::BlockChain& otherChain);
  Roe<void> evaluateCheckpoints();

private:
  struct Config {
    std::string workDir;
    BlockChainConfig chain;
    CheckpointConfig checkpoint;
  };

  // Helper methods
  uint64_t calculateBlockchainSize() const;
  uint64_t getBlockAge(uint64_t blockId) const;
  Roe<void> createCheckpoint(uint64_t blockId);
  Roe<void> pruneOldData(uint64_t checkpointId);
  
  // Genesis and checkpoint processing
  Ledger::ChainNode createGenesisBlock(const BlockChainConfig& config) const;

  // Constants
  static constexpr uint32_t VERSION = 1;

  // Configuration
  Config config_;

  // Stakeholders registry
  std::list<Stakeholder> stakeholders_;
  mutable std::mutex stakeholdersMutex_;

  // State tracking
  uint64_t currentCheckpointId_{ 0 };
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

inline std::ostream& operator<<(std::ostream& os, const Beacon::Stakeholder& stakeholder) {
  os << "Stakeholder{id=\"" << stakeholder.id << "\", "
     << "endpoint=" << stakeholder.endpoint << ", "
     << "stake=" << stakeholder.stake << "}";
  return os;
}

} // namespace pp

#endif // PP_LEDGER_BEACON_H