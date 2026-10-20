#ifndef PP_LEDGER_CHAIN_TYPES_H
#define PP_LEDGER_CHAIN_TYPES_H

#include "ledger/ledger/Account.h"
#include "ledger/ledger/Ledger.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace pp {

struct Checkpoint {
  uint64_t lastId{0};
  uint64_t currentId{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &lastId &currentId;
  }
};

struct CheckpointConfig {
  uint64_t minBlocks{0};
  uint64_t minAgeSeconds{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &minBlocks &minAgeSeconds;
  }
};

struct BlockChainConfig {
  int64_t genesisTime{0};
  uint64_t slotDuration{0};
  uint64_t slotsPerEpoch{0};
  uint64_t maxCustomMetaSize{0};
  uint64_t maxTransactionsPerBlock{0};
  std::vector<uint16_t> minFeeCoefficients;
  uint32_t freeCustomMetaSize{0};
  CheckpointConfig checkpoint;
  uint64_t maxValidationTimespanSeconds{0};
  /** Chain identity included in every transaction signing message. */
  std::string networkId;
  /**
   * Empty-block heartbeat: when the mempool/renewals are idle, a slot leader
   * may seal an empty block if `currentSlot - tip.slot >= heartbeatSlots`.
   * `0` disables empty seals (work-only production). Enforced in
   * `checkBlockBodyPolicy` (seal + Full admission); see BLOCK_PIPELINE.md.
   */
  uint64_t heartbeatSlots{0};
  /**
   * Most native tokens genesis may issue (to reserve, fees included) per
   * epoch, net of anything sent back to it. `0` = no issuance.
   */
  uint64_t maxIssuancePerEpoch{0};
  /**
   * One-time fee every account creation (`T_NEW_USER` after genesis) pays on
   * top of the meta-size fee, to the fee account. The meta-size fee (the same
   * formula renewal charges) pays for the account's first period; each
   * renewal pays for the next. `0` = none.
   */
  uint64_t newAccountFee{0};

  // --- Miner registration and liveness (WIRE_SCHEMA.md) ---
  /** Native balance needed to register, and to be in an epoch's committee. */
  uint64_t minerMinStake{0};
  /** Flat fee each T_MINER_REGISTER pays on top of the meta-size fee, to the fee account. */
  uint64_t minerRegistrationFee{0};
  /** A miner whose last block is this many slots old is due: it must produce (empty if need be)
   *  in a slot it leads. 0 = never due (no liveness deregistration). */
  uint64_t minerDueAfterSlots{0};
  /** Consecutive slots led while due without a block before the miner is deregistered. */
  uint64_t minerMaxMissedSlots{0};
  /** Epochs after a liveness deregistration before the account may register again. */
  uint64_t minerReregisterCooldownEpochs{0};

  /**
   * Most bytes a block may take, packed as stored and sent (`Block::ltsToString`); the ledger RPC
   * is sized from it (B034). `0` = no limit (tests); a genesis sets one (default 4 MiB).
   */
  uint64_t maxBlockBytes{0};

  /**
   * How old a block must be to be final, in seconds of slot time (projects/finality FN001): nodes
   * never undo a final block on their own. A block keeps the bound in force when it was committed
   * (FN002). Between `chain_tx::kMinFinalitySeconds` and `kMaxFinalitySeconds`; 24 hours by default.
   */
  uint64_t finalitySeconds{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &genesisTime &slotDuration &slotsPerEpoch &maxCustomMetaSize
        &maxTransactionsPerBlock &minFeeCoefficients &freeCustomMetaSize
            &checkpoint &maxValidationTimespanSeconds &networkId
                &heartbeatSlots &maxIssuancePerEpoch &newAccountFee &minerMinStake
                    &minerRegistrationFee &minerDueAfterSlots &minerMaxMissedSlots
                        &minerReregisterCooldownEpochs &maxBlockBytes &finalitySeconds;
  }
};

/**
 * Empty-seal policy (genesis `heartbeatSlots`).
 * Requires `currentSlot >= tipSlot` and lag of at least `heartbeatSlots`.
 * `heartbeatSlots == 0` disables. Used by miner production and validators.
 */
inline bool shouldSealEmptyHeartbeat(uint64_t currentSlot, uint64_t tipSlot,
                                     uint64_t heartbeatSlots) {
  if (heartbeatSlots == 0) {
    return false;
  }
  if (currentSlot < tipSlot) {
    return false;
  }
  return (currentSlot - tipSlot) >= heartbeatSlots;
}

/** A config update waiting for its epoch (`T_CONFIG` applies at the next epoch boundary). */
struct PendingChainConfig {
  uint64_t activationEpoch{0};
  BlockChainConfig config;

  template <typename Archive> void serialize(Archive &ar) { ar &activationEpoch &config; }
};

/**
 * Genesis's native balance when an epoch's first block applied: issuance in
 * the epoch so far is this minus the current balance (genesis mints by going
 * negative).
 */
struct IssuanceBaseline {
  uint64_t epoch{0};
  int64_t genesisBalance{0};
};

/**
 * Recovery key set (projects/beacon-authority B004 / B007): the top authority. Its only power is
 * replacing the admin set (the genesis account's keys), announced and applied `delayEpochs`
 * later. Fixed at genesis; `T_CONFIG` cannot change it.
 */
struct RecoverySet {
  /** Raw ML-DSA-65 public keys. */
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{0};
  uint64_t delayEpochs{0};

  template <typename Archive> void serialize(Archive &ar) { ar &publicKeys &minSignatures &delayEpochs; }
};

/**
 * Beacon certificate (beacon-authority B001 / B006): which key signs tips, for which generation.
 * Admin-signed through the record that carries it (generation 1: the genesis record).
 */
struct BeaconCertificate {
  std::string networkId;
  /** 1 at genesis; +1 per promotion. */
  uint64_t generation{0};
  /** Raw ML-DSA-65 public key of the beacon signing key (not the Amp identity). */
  std::string publicKey;
  /**
   * Generation > 1: last height the previous generation may sign (kNoCutover: none, it signs
   * until this certificate commits). In force, the block before the commit
   * (chain_tx::certificateInForce). 0 at genesis.
   */
  uint64_t cutoverHeight{0};
  /**
   * With a cutover: the hash (raw 32 bytes) of block `cutoverHeight`, which the admins vouch for
   * (beacon-authority bp2) — a node may sync up to it with no signed tip, and one holding another
   * block there halts. Empty with no cutover.
   */
  std::string cutoverHash;
  /**
   * Multiaddrs of gateways — relays in front of this generation's beacon, never the beacon itself —
   * where nodes may look for it (bp2). Hints only: nodes trust tips, not addresses.
   */
  std::vector<std::string> dialHints;

  /** No cutover: the previous generation signs until the certificate commits. */
  constexpr static const uint64_t kNoCutover = UINT64_MAX;
  bool hasCutover() const { return cutoverHeight != kNoCutover; }

  template <typename Archive> void serialize(Archive &ar) {
    ar &networkId &generation &publicKey &cutoverHeight &cutoverHash &dialHints;
  }

  /** Versioned form carried in a T_BEACON_CERT record's `meta`. */
  constexpr static const uint32_t VERSION = 2;
  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);
};

/**
 * Recovery announcement (beacon-authority ba7, B004 / B007): the recovery set names a new admin set
 * (the genesis account's keys and threshold). Signed by the recovery set through the T_RECOVERY
 * record that carries it.
 */
struct RecoveryAnnouncement {
  std::string networkId;
  /** Raw ML-DSA-65 public keys of the new admin set. */
  std::vector<std::string> adminPublicKeys;
  uint8_t adminMinSignatures{0};

  template <typename Archive> void serialize(Archive &ar) { ar &networkId &adminPublicKeys &adminMinSignatures; }

  /** Versioned form carried in a T_RECOVERY record's `meta`. */
  constexpr static const uint32_t VERSION = 1;
  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);
};

/** A recovery announced on chain, waiting for its epoch; meanwhile the admin set is frozen. */
struct PendingRecovery {
  RecoveryAnnouncement announcement;
  /** The block that committed the announcement. */
  uint64_t announcedAt{0};
  /** The first block of this epoch or later replaces the admin set, before its records. */
  uint64_t applyEpoch{0};

  template <typename Archive> void serialize(Archive &ar) { ar &announcement &announcedAt &applyEpoch; }
};

/** Who holds authority over the chain: the recovery set, the current beacon certificate, and any
 *  recovery pending (ba7). */
struct ChainAuthority {
  RecoverySet recovery;
  BeaconCertificate beacon;
  bool hasPendingRecovery{false};
  PendingRecovery pendingRecovery;

  template <typename Archive> void serialize(Archive &ar) {
    ar &recovery &beacon &hasPendingRecovery &pendingRecovery;
  }
  /** The pending recovery, if any. */
  std::optional<PendingRecovery> pending() const {
    return hasPendingRecovery ? std::optional<PendingRecovery>(pendingRecovery) : std::nullopt;
  }
  /** Byte-wise equality of the serialized form. */
  bool operator==(const ChainAuthority &other) const;
  bool operator!=(const ChainAuthority &other) const { return !(*this == other); }
};

/**
 * The network a re-genesis comes from, at its block `height` (beacon-authority B030). `header` is
 * that block's header (no records): it hashes to `blockHash` and holds `stateRoot`, so the root is
 * bound to the block (B037) — whoever checks the block hash against the old chain checks the root.
 */
struct ChainPredecessor {
  std::string networkId;
  uint64_t height{0};
  std::string blockHash; ///< raw 32 bytes
  std::string stateRoot; ///< raw 32 bytes: block `height`'s header
  Ledger::Block header;

  bool operator==(const ChainPredecessor &other) const {
    return networkId == other.networkId && height == other.height && blockHash == other.blockHash &&
           stateRoot == other.stateRoot && header.headerToString() == other.header.headerToString();
  }
  template <typename Archive> void serialize(Archive &ar) {
    ar & networkId & height & blockHash & stateRoot & header;
  }
};

/** A system account's keys (reserve, registrar, fee, recycle) an importing genesis sets. */
struct SystemAccountKeys {
  uint64_t id{0};
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{1};

  template <typename Archive> void serialize(Archive &ar) { ar & id & publicKeys & minSignatures; }
};

/**
 * An importing genesis (B030, B032): `accountCount` accounts of `predecessor` follow in import
 * blocks — the predecessor's state root checks them — and the system accounts take `systemKeys`
 * (the genesis account takes the meta's `genesis` keys).
 */
struct GenesisImport {
  ChainPredecessor predecessor;
  uint64_t accountCount{0};
  std::vector<SystemAccountKeys> systemKeys;

  template <typename Archive> void serialize(Archive &ar) { ar & predecessor & accountCount & systemKeys; }
};

struct GenesisAccountMeta {
  constexpr static const uint32_t VERSION = 14; // 14: config.finalitySeconds (projects/finality)

  BlockChainConfig config;
  UserAccount genesis;
  /** Genesis renewals only: a config update not yet in force (T_GENESIS / T_CONFIG carry none;
   *  a T_CONFIG's `config` is itself the update). */
  bool hasPending{false};
  PendingChainConfig pending;
  /** Set at genesis; every later genesis meta (T_CONFIG, genesis renewal) carries it unchanged. */
  ChainAuthority authority;
  /** T_GENESIS of a re-genesis only (B032): the accounts it imports. Later metas carry none. */
  bool hasImport{false};
  GenesisImport import;

  template <typename Archive> void serialize(Archive &ar) {
    ar &config &genesis &hasPending &pending &authority &hasImport &import;
  }

  /** Set config + pending to the state in force for a block of `epoch` (a pending update whose
   *  epoch has come is in force). What a genesis renewal must carry. */
  void setConfigStateAt(const BlockChainConfig &active, const std::optional<PendingChainConfig> &pendingUpdate,
                        uint64_t epoch);
  /** True when config + pending equal those of `other` (byte-wise). */
  bool sameConfigState(const GenesisAccountMeta &other) const;

  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);
};

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config);
std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config);
std::ostream &operator<<(std::ostream &os, const ChainAuthority &authority);

} // namespace pp

#endif
