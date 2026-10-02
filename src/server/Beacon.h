#ifndef PP_LEDGER_BEACON_H
#define PP_LEDGER_BEACON_H

#include "Chain.h"

#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "lib/common/Meta.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Utilities.h"

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
 * - Manage SlotCommittee schedule and stakeholder registry
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
    std::vector<utl::MlDsaKeyPair> genesis;
    std::vector<utl::MlDsaKeyPair> fee;
    std::vector<utl::MlDsaKeyPair> reserve;
    std::vector<utl::MlDsaKeyPair> recycle;

    pp::common::Meta ltsToMeta() const;
  };

  /**
   * A miner account created in the genesis block, so the chain has real stake
   * from block 0 (system accounts never count as stake). Id in the issued
   * range; the operator supplies only public keys (miners keep their keys).
   */
  struct GenesisMiner {
    uint64_t id{0};
    std::vector<std::string> publicKeys; ///< raw ML-DSA-65 public keys
    uint8_t minSignatures{1};
    uint64_t stake{0}; ///< initial native-token balance, taken from reserve
  };

  struct InitConfig {
    // Base configuration
    std::string workDir;
    Chain::BlockChainConfig chain;
    InitKeyConfig key;
    std::vector<GenesisMiner> miners;
  };

  struct MountConfig {
    std::string workDir;
  };

  Beacon();
  ~Beacon() override = default;

  // ----------------- accessors -------------------------------------
  Chain::Checkpoint getCheckpoint() const;
  uint64_t getNextBlockId() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;

  Roe<Ledger::ChainNode> readBlock(uint64_t blockId) const;
  std::string calculateHash(const Ledger::Block &block) const;
  /** networkId from loaded chain config (empty if not ready). */
  std::string getNetworkId() const;
  /**
   * A REGISTER is genuine only if the miner account's keys signed it (as for
   * its transactions) and its endpoint's /p2p/ peer id is one of those keys:
   * nobody can register an address for a miner id they do not control.
   */
  Roe<void> verifyMinerRegistration(const Client::MinerInfo &miner, const std::string &networkId) const;
  /** Find transactions involving walletId, scanning backwards from ioBlockId (0 = latest). ioBlockId is updated to the last block scanned. */
  Roe<std::vector<Ledger::Record>>
  findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const;
  /** Find transaction by global chain index (0-based). */
  Roe<Ledger::Record>
  findTransactionByIndex(uint64_t txIndex) const;

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
                     const InitKeyConfig &key,
                     const std::vector<GenesisMiner> &miners);
  /** Genesis miner NEW_USER records (after the four system records) and their total stake + fees. */
  Roe<std::pair<std::vector<Ledger::Record>, int64_t>>
  createGenesisMinerRecords(const Chain::BlockChainConfig &config, const InitKeyConfig &key,
                            const std::vector<GenesisMiner> &miners);

  /** Signs transaction with genesis keys and adds signatures. Returns error on
   * sign failure. */
  Roe<void>
  signWithGenesisKeys(Ledger::Record &record,
                      const std::vector<utl::MlDsaKeyPair> &genesisKeys,
                      const std::string &networkId,
                      const std::string &errorContext) const;

  Chain chain_;
  Config config_;
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Beacon::InitConfig &config);
std::ostream &operator<<(std::ostream &os, const Beacon::MountConfig &config);
} // namespace pp

#endif // PP_LEDGER_BEACON_H
