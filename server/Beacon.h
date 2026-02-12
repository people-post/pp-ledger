#ifndef PP_LEDGER_BEACON_H
#define PP_LEDGER_BEACON_H

#include "Chain.h"

#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../lib/Utilities.h"
#include "../network/Types.hpp"

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <vector>

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
class Beacon : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  struct InitKeyConfig {
    std::vector<utl::Ed25519KeyPair> genesis;
    std::vector<utl::Ed25519KeyPair> fee;
    std::vector<utl::Ed25519KeyPair> reserve;
    std::vector<utl::Ed25519KeyPair> recycle;

    nlohmann::json toJson() const;
  };

  struct InitConfig {
    // Base configuration
    std::string workDir;
    Chain::BlockChainConfig chain;
    InitKeyConfig key;
  };

  struct MountConfig {
    std::string workDir;
  };

  Beacon();
  ~Beacon() override = default;

  // ----------------- accessors -------------------------------------
  uint64_t getLastCheckpointId() const;
  uint64_t getCurrentCheckpointId() const;
  uint64_t getNextBlockId() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Ledger::ChainNode> getBlock(uint64_t blockId) const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;
  std::string calculateHash(const Ledger::Block &block) const;

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig &config);
  Roe<void> mount(const MountConfig &config);
  void refresh();

  Roe<void> addBlock(const Ledger::ChainNode &block);

private:
  constexpr static const char *DIR_LEDGER = "ledger";

  struct Config {
    std::string workDir;
  };

  Roe<Ledger::ChainNode>
  createGenesisBlock(const Chain::BlockChainConfig &config,
                     const InitKeyConfig &key) const;

  /** Signs transaction with genesis keys and adds signatures. Returns error on
   * sign failure. */
  Roe<void>
  signWithGenesisKeys(Ledger::SignedData<Ledger::Transaction> &signedTx,
                      const std::vector<utl::Ed25519KeyPair> &genesisKeys,
                      const std::string &errorContext) const;

  Chain chain_;
  Config config_;
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Beacon::InitConfig &config);
std::ostream &operator<<(std::ostream &os, const Beacon::MountConfig &config);
} // namespace pp

#endif // PP_LEDGER_BEACON_H
