#ifndef PP_LEDGER_BEACON_H
#define PP_LEDGER_BEACON_H

#include "ledger/server/NodeCore.h"
#include "ledger/chain/Genesis.h"
#include "ledger/server/TipSigner.h"

#include "ledger/lib/common/Meta.h"
#include "ledger/lib/common/Utilities.h"

#include <cstdint>
#include <functional>
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
class Beacon : public NodeCore {
public:
  /** An account's public keys and how many must sign (0 = all of them). */
  using AccountKeys = chain_genesis::AccountKeys;

  /**
   * Genesis accounts by public key (init-config.json `systemAccounts`), plus
   * the genesis private keys that sign the genesis block (`--genesis-key`).
   * The beacon generates and keeps no account keys: each holder creates its
   * own; the genesis key is the long-term admin key, kept offline.
   */
  struct InitKeyConfig : chain_genesis::SystemKeys {
    std::vector<std::string> genesisSigners; ///< raw ML-DSA-65 private keys
    /** The system accounts' domains (init-config.json `systemNames`; projects/domains). */
    chain_genesis::SystemNames names = chain_genesis::defaultSystemNames();
  };

  /** A miner account created in the genesis block (chain_genesis::Miner). */
  using GenesisMiner = chain_genesis::Miner;

  struct InitConfig {
    // Base configuration
    std::string workDir;
    Chain::BlockChainConfig chain;
    InitKeyConfig key;
    std::vector<GenesisMiner> miners;
    /** Recovery set + generation-1 beacon certificate (its networkId is set from `chain`). */
    Chain::ChainAuthority authority;
    /**
     * A re-genesis (beacon-authority B032): the predecessor's accounts, imported in blocks 1..k
     * after a genesis that carries only the config, keys and authority (no `miners`).
     */
    std::optional<StateSnapshot> import;
    /**
     * Block 0's records signed offline (a genesis operation file): written as they are, at the
     * genesis time their meta names; `chain`, `key`, `miners` and `authority` are then unused.
     */
    std::vector<Ledger::Record> signedGenesis;
  };

  struct MountConfig {
    std::string workDir;
  };

  Beacon();
  ~Beacon() override = default;

  // ----------------- accessors -------------------------------------
  /** Live admission only: a block for `slot` arrives before slotEnd + delivery window. */
  bool isBeforeDeliveryDeadline(uint64_t slot) const;
  /**
   * A REGISTER is genuine only if the miner account's keys signed it (as for
   * its transactions): nobody can register a miner id they do not control.
   */
  Roe<void> verifyMinerRegistration(const Client::MinerInfo &miner, const std::string &networkId) const;

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig &config);
  Roe<void> mount(const MountConfig &config);

  /**
   * Commit `block`; once tip signing is open, sign it (sign after durable commit). A block that
   * commits but cannot be signed still counts: the error is logged and the tip stays where it was.
   */
  Roe<void> addBlock(const Ledger::ChainNode &block);

  /**
   * Start signing tips with `key` (after mount; beacon-authority ba3). Loads the persisted tip and
   * refuses a ledger that ends below it or differs from it — the beacon would then sign a second
   * block at a height it already signed. Signs the committed tip if it is not signed yet (the
   * genesis block after --init, or a crash between commit and signing).
   *
   * No persisted tip while it holds the key in force and its ledger holds blocks past genesis: a
   * work directory moved without `data/signed-tip.dat`, or with a ledger older than the tip the
   * last host signed (beacon-authority bp4). Refused unless `acceptLedgerTip` (the operator knows
   * this ledger holds every block the key signed): signing its tip could sign a height a second
   * time. Not refused when this start's rewind dropped the tip, or while taking over.
   */
  Roe<void> openTipSigning(const std::vector<utl::MlDsaKeyPair> &keys, bool acceptLedgerTip = false);
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
   * Taking over (bp3): a certificate held out of band names this beacon's key, and the one in force
   * does not — a relay promoted to be the new beacon. It serves reads and STATUS (the certificate, no
   * signed tip) and takes registrations and transactions; it commits nothing above the cutover but
   * the block committing a certificate, and signs from there. Its generation; none otherwise.
   */
  std::optional<uint64_t> takingOver() const;
  /** Whether a beacon holding `keys` would take over (takingOver), before it opens tip signing. */
  std::optional<uint64_t> takeoverWith(const std::vector<utl::MlDsaKeyPair> &keys) const;
  /** The latest signed tip's file under the mount's work directory (beacon only). */
  static constexpr const char *FILE_SIGNED_TIP = "signed-tip.dat";
  /** Block C here is not the one the held certificate vouches for (Chain::cutoverForked). */
  bool cutoverForked() const { return chain_.cutoverForked(); }
  /**
   * Equivocation evidence held (beacon-authority ba6): its height. The beacon then commits no block
   * above it and takes no producer writes until a later generation is certified (resume).
   */
  std::optional<uint64_t> haltedAbove() const;
  /** Equivocation evidence pushed up (EQUIVOCATION_ADD): kept and persisted when it verifies. */
  Roe<Chain::EquivocationOffer> offerEquivocation(const Client::Equivocation &evidence);
  /**
   * A beacon certificate from outside the blocks (TX_ADD, beacon-authority ba5c): kept and
   * persisted if it is the highest generation seen. While it is not on chain, the beacon commits
   * no block above its cutover that does not commit a certificate, and stands down if it does
   * not hold its key.
   */
  Roe<Chain::CertificateOffer> offerCertificate(const Ledger::Record &record);

protected:
  /** A tip above the kept blocks was signed by the superseded generation: drop it (dropSignedTipAbove). */
  Roe<void> onRewound(uint64_t keptUpTo) override;

private:
  /** Block 0 of a re-genesis: T_GENESIS only, naming the predecessor and the system keys (B032). */
  Roe<Ledger::ChainNode> createImportingGenesisBlock(const Chain::BlockChainConfig &config, const InitKeyConfig &key,
                                                     const Chain::ChainAuthority &authority,
                                                     const StateSnapshot &snapshot);
  /** Blocks 1..k of a re-genesis: the snapshot's accounts, in order, each block within maxBlockBytes. */
  Roe<void> addImportBlocks(const StateSnapshot &snapshot, int64_t genesisTime, uint64_t maxBlockBytes);

  Roe<Ledger::ChainNode>
  createGenesisBlock(const Chain::BlockChainConfig &config,
                     const InitKeyConfig &key,
                     const std::vector<GenesisMiner> &miners,
                     const Chain::ChainAuthority &authority);
  /** Block 0 of the records signed offline (InitConfig::signedGenesis). */
  Roe<Ledger::ChainNode> createSignedGenesisBlock(const std::vector<Ledger::Record> &records);
  /** Seal block 0 of `records` at `genesisTime`. */
  Roe<Ledger::ChainNode> sealGenesisBlock(std::vector<Ledger::Record> records, int64_t genesisTime);

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
  /** openTipSigning with no persisted tip: refuse a ledger past genesis it would sign (bp4). */
  Roe<void> checkUnsignedLedger(const TipSigner &signer, bool acceptLedgerTip) const;
  /** Refuse a ledger that ends below the persisted tip or holds another block at its height. */
  Roe<void> checkLedgerHoldsSignedTip(const Client::SignedTip &tip) const;
  std::optional<uint64_t> takeoverFor(const std::function<bool(const std::string &publicKey)> &holds) const;
  /** Sign the committed tip under the certificate in force. */
  TipSigner::Roe<void> signCommittedTip();

  std::unique_ptr<TipSigner> tipSigner_;
  /** Generation already reported as certifying another key (one report per generation). */
  uint64_t uncertifiedWarnedGeneration_{0};
  /** This start's rewind removed the signed tip (dropSignedTipAbove): no tip is expected. */
  bool droppedTipThisStart_{false};
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Beacon::InitConfig &config);
std::ostream &operator<<(std::ostream &os, const Beacon::MountConfig &config);
} // namespace pp

#endif // PP_LEDGER_BEACON_H
