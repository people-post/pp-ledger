#ifndef PP_LEDGER_BEACON_H
#define PP_LEDGER_BEACON_H

#include "Validator.h"
#include "../ledger/Ledger.h"
#include "../consensus/Ouroboros.h"
#include "../network/Types.hpp"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../lib/Utilities.h"

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
    std::vector<utl::Ed25519KeyPair> genesis;
    std::vector<utl::Ed25519KeyPair> fee;
    std::vector<utl::Ed25519KeyPair> reserve;

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

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig& config);
  Roe<void> mount(const MountConfig& config);
  void refresh();

  Roe<void> addBlock(const Ledger::ChainNode& block);

private:
  constexpr static const char* DIR_LEDGER = "ledger";

  struct Config {
    std::string workDir;
  };

  Roe<Ledger::ChainNode> createGenesisBlock(const BlockChainConfig& config, const InitKeyConfig& key) const;

  Config config_;
};

// Ostream operators for easy logging
std::ostream& operator<<(std::ostream& os, const Beacon::InitConfig& config);
std::ostream& operator<<(std::ostream& os, const Beacon::MountConfig& config);
} // namespace pp

#endif // PP_LEDGER_BEACON_H