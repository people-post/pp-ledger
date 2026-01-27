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

  struct InitConfig {
    // Base configuration
    std::string workDir;
    uint64_t slotDuration = 1; // seconds
    uint64_t slotsPerEpoch = 21600; // ~6 hours
    
    // Checkpoint configuration
    uint64_t checkpointMinSizeBytes = 1024ULL * 1024 * 1024; // 1GB default
    uint64_t checkpointAgeSeconds = 365ULL * 24 * 3600; // 1 year default
  };

  struct Stakeholder {
    std::string id;
    network::TcpEndpoint endpoint;
    uint64_t stake;
  };

  Beacon();
  ~Beacon() override = default;

  // Initialization
  Roe<void> init(const InitConfig& config);
  Roe<void> mount(const std::string& workDir);

  // Version and metadata
  uint32_t getVersion() const { return VERSION; }
  uint64_t getCurrentCheckpointId() const;
  uint64_t getTotalStake() const;
  
  // Stakeholder management
  const std::list<Stakeholder>& getStakeholders() const;
  void addStakeholder(const Stakeholder& stakeholder);
  void removeStakeholder(const std::string& stakeholderId);
  void updateStake(const std::string& stakeholderId, uint64_t newStake);

  // Block operations (override base class)
  Roe<void> addBlock(const Ledger::ChainNode& block);
  Roe<void> validateBlock(const Ledger::ChainNode& block) const;

  // Chain synchronization
  Roe<void> syncChain(const Validator::BlockChain& otherChain);
  Roe<bool> shouldAcceptChain(const Validator::BlockChain& candidateChain) const;

  // Checkpoint management
  Roe<void> evaluateCheckpoints();
  Roe<std::vector<uint64_t>> getCheckpoints() const;
  bool needsCheckpoint() const;

  // Consensus queries
  Roe<std::string> getSlotLeader(uint64_t slot) const;

private:
  struct Config {
    std::string workDir;
    uint64_t slotDuration{ 0 };
    uint64_t slotsPerEpoch{ 0 };
    uint64_t checkpointMinSizeBytes{ 0 };
    uint64_t checkpointAgeSeconds{ 0 };
  };

  // Helper methods
  uint64_t calculateBlockchainSize() const;
  uint64_t getBlockAge(uint64_t blockId) const;
  Roe<void> createCheckpoint(uint64_t blockId);
  Roe<void> pruneOldData(uint64_t checkpointId);

  // Constants
  static constexpr uint32_t VERSION = 1;

  // Configuration
  Config config_;

  // Stakeholders registry
  std::list<Stakeholder> stakeholders_;
  mutable std::mutex stakeholdersMutex_;

  // State tracking
  uint64_t currentCheckpointId_;
};

} // namespace pp

#endif // PP_LEDGER_BEACON_H