#ifndef PP_LEDGER_BEACON_H
#define PP_LEDGER_BEACON_H

#include "CertificateStore.h"
#include "Rewind.h"
#include "EquivocationStore.h"
#include "Chain.h"
#include "TipSigner.h"

#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "lib/common/Meta.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Utilities.h"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
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

  /** An account's public keys and how many must sign (0 = all of them). */
  struct AccountKeys {
    std::vector<std::string> publicKeys; ///< raw ML-DSA-65 public keys
    uint8_t minSignatures{0};
  };

  /**
   * Genesis accounts by public key (init-config.json `systemAccounts`), plus
   * the genesis private keys that sign the genesis block (`--genesis-key`).
   * The beacon generates and keeps no account keys: each holder creates its
   * own; the genesis key is the long-term admin key, kept offline.
   */
  struct InitKeyConfig {
    AccountKeys genesis;
    AccountKeys reserve;
    AccountKeys registrar;
    AccountKeys fee;
    AccountKeys recycle;
    std::vector<std::string> genesisSigners; ///< raw ML-DSA-65 private keys
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
    /** Recovery set + generation-1 beacon certificate (its networkId is set from `chain`). */
    Chain::ChainAuthority authority;
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
  /** Live admission only: a block for `slot` arrives before slotEnd + delivery window. */
  bool isBeforeDeliveryDeadline(uint64_t slot) const;
  uint64_t getCurrentEpoch() const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;

  Roe<Ledger::ChainNode> readBlock(uint64_t blockId) const;
  /** AUTHORITY_PROOF up to `upToBlockId` (Chain::authorityProof). */
  Roe<Client::AuthorityProof> authorityProof(uint64_t upToBlockId) const;
  std::string calculateHash(const Ledger::Block &block) const;
  /** networkId from loaded chain config (empty if not ready). */
  std::string getNetworkId() const;
  /**
   * A REGISTER is genuine only if the miner account's keys signed it (as for
   * its transactions): nobody can register a miner id they do not control.
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

  /**
   * Commit `block`; once tip signing is open, sign it (sign after durable commit). A block that
   * commits but cannot be signed still counts: the error is logged and the tip stays where it was.
   */
  Roe<void> addBlock(const Ledger::ChainNode &block);

  /**
   * At start, before the ledger mounts: apply a rewind ordered before the restart
   * (beacon-authority bp1, B022) and load the last one applied.
   */
  Roe<void> applyPendingRewind(const std::string &ledgerDir);
  /**
   * When the held certificate orders a rewind (Chain::rewindOrder), write the order for the next
   * start; true when one was written — the caller restarts.
   */
  Roe<bool> scheduleRewind();
  /** The last rewind this node applied (STATUS); none if none. */
  const std::optional<Client::RewindReport> &getLastRewind() const { return lastRewind_; }

  /**
   * Start signing tips with `key` (after mount; beacon-authority ba3). Loads the persisted tip and
   * refuses a ledger that ends below it or differs from it — the beacon would then sign a second
   * block at a height it already signed. Signs the committed tip if it is not signed yet (the
   * genesis block after --init, or a crash between commit and signing).
   */
  Roe<void> openTipSigning(const std::vector<utl::MlDsaKeyPair> &keys);
  /** The latest signed tip (none before openTipSigning, or while this key is not certified). */
  std::optional<Client::SignedTip> getSignedTip() const;
  /**
   * The certificate in force names a key this beacon does not hold (beacon-authority ba5), or a
   * later certificate seen out of band does (ba5c): that generation belongs to another beacon, so
   * this one stands down — it stops committing and serves reads only. False before openTipSigning.
   */
  bool isSuperseded() const { return supersededBy().has_value(); }
  /** The generation that certifies a key this beacon does not hold; none if it is not superseded. */
  std::optional<uint64_t> supersededBy() const;
  /**
   * Equivocation evidence held (beacon-authority ba6): its height. The beacon then commits no block
   * above it and takes no producer writes until a later generation is certified (resume).
   */
  std::optional<uint64_t> haltedAbove() const;
  /** Equivocation evidence pushed up (EQUIVOCATION_ADD): kept and persisted when it verifies. */
  Roe<Chain::EquivocationOffer> offerEquivocation(const Client::Equivocation &evidence);
  /** The beacon generation in force (0 before the chain is loaded). */
  uint64_t getGeneration() const;

  /**
   * A beacon certificate from outside the blocks (TX_ADD, beacon-authority ba5c): kept and
   * persisted if it is the highest generation seen. While it is not on chain, the beacon commits
   * no block above its cutover that does not commit a certificate, and stands down if it does
   * not hold its key.
   */
  Roe<Chain::CertificateOffer> offerCertificate(const Ledger::Record &record);
  /** The current certificate for STATUS (Chain::currentCertificateRecord). */
  std::optional<Ledger::Record> getCurrentCertificate() const;
  /** Equivocation evidence this node halts on (beacon-authority ba6), for STATUS; none if none. */
  std::optional<Client::Equivocation> getEquivocation() const { return chain_.equivocation(); }
  /** A recovery pending on this chain (beacon-authority ba7), for STATUS; none if none. */
  std::optional<Client::RecoveryStatus> getRecoveryStatus() const { return chain_.recoveryStatus(); }

private:
  constexpr static const char *DIR_LEDGER = "ledger";
  /** The latest signed tip, beside the ledger. */
  constexpr static const char *FILE_SIGNED_TIP = "signed-tip.dat";
  /** The highest beacon certificate seen out of band, beside the ledger. */
  constexpr static const char *FILE_BEACON_CERT = "beacon-cert.dat";
  /** Equivocation evidence this node halts on (ba6), beside the ledger. */
  constexpr static const char *FILE_EQUIVOCATION = "equivocation.dat";

  struct Config {
    std::string workDir;
  };

  Roe<Ledger::ChainNode>
  createGenesisBlock(const Chain::BlockChainConfig &config,
                     const InitKeyConfig &key,
                     const std::vector<GenesisMiner> &miners,
                     const Chain::ChainAuthority &authority);
  /**
   * A genesis-signed NEW_USER record creating `account` at `toId`. `fee` 0
   * means "the minimum fee for this record"; the fee used is written back.
   */
  Roe<Ledger::Record> createSystemAccountRecord(const Chain::BlockChainConfig &config, const InitKeyConfig &key,
                                                uint64_t toId, const Client::UserAccount &account,
                                                uint64_t &fee, const std::string &label);
  /** Reserve's NEW_USER record: the supply left after `otherCosts` (other records' fees and stakes). */
  Roe<Ledger::Record> createReserveRecord(const Chain::BlockChainConfig &config, const InitKeyConfig &key,
                                          int64_t otherCosts);
  /** Genesis miner NEW_USER records (after the system records) and their total stake + fees. */
  Roe<std::pair<std::vector<Ledger::Record>, int64_t>>
  createGenesisMinerRecords(const Chain::BlockChainConfig &config, const InitKeyConfig &key,
                            const std::vector<GenesisMiner> &miners);

  /**
   * The signers must be distinct genesis keys and at least the genesis
   * account's minSignatures; every account needs keys (minSignatures <= count).
   */
  Roe<void> checkInitKeys(const InitKeyConfig &key) const;

  /** Signs transaction with genesis keys and adds signatures. Returns error on
   * sign failure. */
  Roe<void>
  signWithGenesisKeys(Ledger::Record &record,
                      const std::vector<std::string> &genesisSigners,
                      const std::string &networkId,
                      const std::string &errorContext) const;

  /** After a rewind: remove a persisted tip above `height` (signed by a superseded generation). */
  Roe<void> dropSignedTipAbove(uint64_t height);
  /** Refuse a ledger that ends below the persisted tip or holds another block at its height. */
  Roe<void> checkLedgerHoldsSignedTip(const Client::SignedTip &tip) const;
  /** Sign the committed tip under the certificate in force. */
  TipSigner::Roe<void> signCommittedTip();

  Chain chain_;
  Config config_;
  std::unique_ptr<TipSigner> tipSigner_;
  std::unique_ptr<CertificateStore> certificates_;
  std::optional<Client::RewindReport> lastRewind_;
  std::unique_ptr<EquivocationStore> equivocations_;
  /** Generation already reported as certifying another key (one report per generation). */
  uint64_t uncertifiedWarnedGeneration_{0};
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Beacon::InitConfig &config);
std::ostream &operator<<(std::ostream &os, const Beacon::MountConfig &config);
} // namespace pp

#endif // PP_LEDGER_BEACON_H
