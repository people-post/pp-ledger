#ifndef PP_LEDGER_CHAIN_H
#define PP_LEDGER_CHAIN_H

#include "../client/Client.h"
#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "AccountBuffer.h"
#include "BlockValidation.h"
#include "ErrorCodes.h"
#include "RecordHandler.h"
#include "TxContext.h"
#include "TxError.h"
#include "Types.h"
#include "AuthorityTrail.h"
#include "lib/common/Crypto.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Utilities.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace pp {

/**
 * Chain - Core block validation and chain management
 *
 * Provides common functionality for:
 * - Block validation
 * - Chain management
 * - Consensus integration
 * - Ledger operations
 */
class Chain : public Module {
public:
  using Checkpoint = ::pp::Checkpoint;
  using CheckpointConfig = ::pp::CheckpointConfig;
  using BlockChainConfig = ::pp::BlockChainConfig;
  using GenesisAccountMeta = ::pp::GenesisAccountMeta;
  using ChainAuthority = ::pp::ChainAuthority;
  using BeaconCertificate = ::pp::BeaconCertificate;

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
    Error(const chain_tx::TxError &e) : RoeErrorBase(e.code, e.message) {}
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Error code groups
  constexpr static int32_t E_STATE_INIT = chain_err::E_STATE_INIT;
  constexpr static int32_t E_STATE_MOUNT = chain_err::E_STATE_MOUNT;
  constexpr static int32_t E_INVALID_ARGUMENT = chain_err::E_INVALID_ARGUMENT;

  constexpr static int32_t E_BLOCK_NOT_FOUND = chain_err::E_BLOCK_NOT_FOUND;
  constexpr static int32_t E_BLOCK_SEQUENCE = chain_err::E_BLOCK_SEQUENCE;
  constexpr static int32_t E_BLOCK_HASH = chain_err::E_BLOCK_HASH;
  constexpr static int32_t E_BLOCK_INDEX = chain_err::E_BLOCK_INDEX;
  constexpr static int32_t E_BLOCK_CHAIN = chain_err::E_BLOCK_CHAIN;
  constexpr static int32_t E_BLOCK_VALIDATION = chain_err::E_BLOCK_VALIDATION;
  constexpr static int32_t E_BLOCK_GENESIS = chain_err::E_BLOCK_GENESIS;

  constexpr static int32_t E_CONSENSUS_SLOT_LEADER =
      chain_err::E_CONSENSUS_SLOT_LEADER;
  constexpr static int32_t E_CONSENSUS_TIMING = chain_err::E_CONSENSUS_TIMING;
  constexpr static int32_t E_CONSENSUS_QUERY = chain_err::E_CONSENSUS_QUERY;

  constexpr static int32_t E_ACCOUNT_NOT_FOUND = chain_err::E_ACCOUNT_NOT_FOUND;
  constexpr static int32_t E_ACCOUNT_EXISTS = chain_err::E_ACCOUNT_EXISTS;
  constexpr static int32_t E_ACCOUNT_BALANCE = chain_err::E_ACCOUNT_BALANCE;
  constexpr static int32_t E_ACCOUNT_BUFFER = chain_err::E_ACCOUNT_BUFFER;
  constexpr static int32_t E_ACCOUNT_RENEWAL = chain_err::E_ACCOUNT_RENEWAL;

  constexpr static int32_t E_TX_VALIDATION = chain_err::E_TX_VALIDATION;
  constexpr static int32_t E_TX_SIGNATURE = chain_err::E_TX_SIGNATURE;
  constexpr static int32_t E_TX_FEE = chain_err::E_TX_FEE;
  constexpr static int32_t E_TX_AMOUNT = chain_err::E_TX_AMOUNT;
  constexpr static int32_t E_TX_TYPE = chain_err::E_TX_TYPE;
  constexpr static int32_t E_TX_TRANSFER = chain_err::E_TX_TRANSFER;
  constexpr static int32_t E_TX_IDEMPOTENCY = chain_err::E_TX_IDEMPOTENCY;
  constexpr static int32_t E_TX_VALIDATION_TIMESPAN =
      chain_err::E_TX_VALIDATION_TIMESPAN;
  constexpr static int32_t E_TX_TIME_OUTSIDE_WINDOW =
      chain_err::E_TX_TIME_OUTSIDE_WINDOW;

  constexpr static int32_t E_LEDGER_WRITE = chain_err::E_LEDGER_WRITE;
  constexpr static int32_t E_LEDGER_READ = chain_err::E_LEDGER_READ;

  constexpr static int32_t E_INTERNAL_DESERIALIZE =
      chain_err::E_INTERNAL_DESERIALIZE;
  constexpr static int32_t E_INTERNAL_BUFFER = chain_err::E_INTERNAL_BUFFER;
  constexpr static int32_t E_INTERNAL = chain_err::E_INTERNAL;

  Chain();
  ~Chain() override = default;

  // ----------------- accessors -------------------------------------
  bool isStakeholderSlotLeader(uint64_t stakeholderId, uint64_t slot) const;
  /** See SlotCommittee::isSlotSealTime / isBeforeDeliveryDeadline / getProductionSlot. */
  bool isSlotSealTime(uint64_t slot) const;
  /** True when `accountId` is a registered miner due at `slot` (must produce, empty if need be). */
  bool isMinerDue(uint64_t accountId, uint64_t slot) const;
  bool isBeforeDeliveryDeadline(uint64_t slot) const;
  uint64_t getProductionSlot() const;
  /** True when chain config has been loaded (from T_GENESIS or T_CONFIG). */
  bool isChainConfigReady() const;
  /**
   * Whether this node may seal a block at `slot` (beacon-authority ba5b): it validates blocks Full
   * (it is not warming up from a checkpoint), and the blocks it holds reach back over the longest
   * transaction validity window, so a transaction already committed cannot pass its idempotency
   * check again. A node started at a checkpoint waits until its history covers that window.
   */
  Roe<void> sealReadiness(uint64_t slot) const;


  uint64_t getNextBlockId() const;
  Checkpoint getCheckpoint() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  uint64_t getTotalStake() const;
  Roe<uint64_t> getSlotLeader(uint64_t slot) const;
  uint64_t getEpochFromSlot(uint64_t slot) const {
    return txContext_.consensus.getEpochFromSlot(slot);
  }
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;
  /** `signatures` over `message` satisfy account `accountId`'s keys and minSignatures. */
  Roe<void> verifyAccountSignatures(uint64_t accountId, const std::string &message,
                                    const std::vector<std::string> &signatures) const;
  int64_t getConsensusTimestamp() const;
  int64_t getConsensusTimestampMs() const;
  /** End of `slot` (seconds) — the earliest timestamp a block for it may carry. */
  int64_t getSlotEndTime(uint64_t slot) const;
  /** Start time of the given slot (consensus timestamp). */
  int64_t getSlotStartTime(uint64_t slot) const;
  /** Slot duration in seconds. */
  uint64_t getSlotDuration() const;
  uint64_t getStakeholderStake(uint64_t stakeholderId) const;
  /** Max transactions per block (0 = no limit). Renewals are not counted toward
   * this cap. */
  uint64_t getMaxTransactionsPerBlock() const;
  /**
   * Empty heartbeat lag in slots from genesis config (`0` = empty seals off).
   */
  uint64_t getHeartbeatSlots() const;
  /** networkId from loaded chain config (empty if config not ready). */
  std::string getNetworkId() const;
  /** Recovery set + beacon certificate in force; nullopt before genesis is applied. */
  std::optional<ChainAuthority> getAuthority() const { return txContext_.optAuthority; }
  /**
   * A T_BEACON_CERT record received outside a block (a peer, a file): signed by the admin set in
   * force, for this chain, a higher generation than the one in force, with a new key. Returns the
   * certificate. (On chain the record must also be exactly the next generation.)
   */
  Roe<BeaconCertificate> verifyBeaconCertificateRecord(const Ledger::Record &record) const;

  /** What offerCertificate did with a certificate (beacon-authority ba5c). */
  enum class CertificateOffer {
    Newer,    ///< the highest generation seen: kept
    Same,     ///< already held
    Older,    ///< not above the one held or the generation in force: ignored
    Conflict, ///< another certificate of the generation held, also admin-signed: the held one stays
  };
  /**
   * A T_BEACON_CERT record from outside the blocks (STATUS, a peer, an operator's file), verified
   * like verifyBeaconCertificateRecord and kept if it is the highest generation seen. Until a block
   * commits that generation, tips of older generations above its cutover are refused
   * (authorityTrail()). Dropped once a block commits its generation (or a later one), or when an
   * admin rotation leaves it unsigned. In memory only: the caller persists it.
   */
  Roe<CertificateOffer> offerCertificate(const Ledger::Record &record);
  /** The certificate held out of band, above the generation in force; none if none is. */
  const std::optional<Ledger::Record> &seenCertificateRecord() const { return seenCertRecord_; }
  const std::optional<BeaconCertificate> &seenCertificate() const { return seenCert_; }
  /**
   * The current certificate, as STATUS serves it: the one held out of band, else the latest
   * committed T_BEACON_CERT this node holds (none at generation 1, or from before a checkpoint
   * start).
   */
  std::optional<Ledger::Record> currentCertificateRecord() const;
  /**
   * The beacon's commit rule while it holds a certificate out of band (ba5c): a block above its
   * cutover must commit a beacon certificate. The generation in force may sign nothing above the
   * cutover, and the certificate commits no later than the block after it.
   */
  Roe<void> checkSeenCutover(const Ledger::Block &block) const;

  /**
   * The beacon authority from the committed tip on (beacon-authority ba5): observe blocks not
   * committed yet to follow generation and admin changes, then verify a tip against them — bound
   * by the cutover of a certificate held out of band (ba5c).
   */
  Roe<AuthorityTrail> authorityTrail() const;

  /**
   * The records that changed the authority from genesis up to `upToBlockId` (AUTHORITY_PROOF,
   * beacon-authority ba5b). Only a chain loaded from genesis holds them all.
   */
  Roe<Client::AuthorityProof> authorityProof(uint64_t upToBlockId) const;

  /**
   * A beacon signed tip of this chain, signed by the generation in force here — or, given the
   * blocks after the committed tip that it covers (`pending`), the generation in force after them.
   */
  Roe<void> verifySignedTip(const Client::SignedTip &tip, const std::vector<const Ledger::Block *> &pending = {}) const;

  // ----------------- methods -------------------------------------
  std::string calculateHash(const Ledger::Block &block) const;

  /**
   * Apply `block.records` once to the tip bank, fill header commitments
   * (`epoch`, `stakeSnapshotHash`, `txRoot`, `stateRoot`) and `hash`.
   * Does not persist; the matching `addBlock` only writes the ledger.
   * Must not be called again until that `addBlock` (or the tip is reset).
   * On failure (here or in the matching commit) tip state is rolled back.
   */
  Roe<void> sealBlock(Ledger::ChainNode &block);
  /** Drop an uncommitted seal (e.g. broadcast failed) and restore the tip. */
  void abandonSeal();

  /**
   * Assemble (producer half): next-block link fields from `previous`.
   * Does not fill epoch / stake / seed / txRoot — `sealBlock` →
   * `assembleBlockHeader` does that. See BLOCK_PIPELINE.md.
   */
  Ledger::ChainNode linkNextBlock(const Ledger::ChainNode &previous,
                                  uint64_t slot, uint64_t slotLeader,
                                  int64_t timestamp,
                                  std::vector<Ledger::Record> records) const;

  Roe<std::vector<Ledger::Record>>
  collectRenewals(uint64_t slot) const;

  Roe<Ledger::ChainNode> readBlock(uint64_t blockId) const;
  Roe<Ledger::ChainNode> readLastBlock() const;

  Roe<uint64_t>
  calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                    const Ledger::TypedTx &tx) const;

  Roe<std::vector<Ledger::Record>>
  findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const;
  Roe<Ledger::Record>
  findTransactionByIndex(uint64_t txIndex) const;

  Roe<void>
  addBufferTransaction(AccountBuffer &bank,
                       const Ledger::Record &record,
                       uint64_t slotLeaderId) const;

  void initConsensus(const consensus::SlotCommittee::Config &config);

  /**
   * Test/sim injectors forwarded to SlotCommittee (see docs/architecture/TESTING.md).
   * Production binaries must not call these.
   */
  void setClockOverride(std::optional<int64_t> unixSeconds);
  void forceSlotLeader(uint64_t slot, uint64_t stakeholderId);
  void clearForcedSlotLeaders();
  /** Install or derive SlotCommittee epoch seed for `epoch` (no-op if already set). */
  Roe<void> ensureEpochSeed(uint64_t epoch);
  /**
   * Block path: seed for the block's epoch. At the first block of an epoch the
   * previous epoch is complete on this chain, so any cached seed (possibly
   * derived early by the wall-clock refresh) is re-derived.
   */
  Roe<void> ensureBlockEpochSeed(const Ledger::ChainNode &block);
  const std::string &getEpochSeed() const {
    return txContext_.consensus.getEpochSeed();
  }

  Roe<void> initLedger(const Ledger::InitConfig &config);
  Roe<void> mountLedger(const std::string &workDir);
  Roe<uint64_t> loadFromLedger(uint64_t startingBlockId);
  Roe<void> addBlock(const Ledger::ChainNode &block);
  /**
   * Live mode (wall-clock epoch): install the epoch's recorded stake snapshot,
   * or a provisional one for leader election that is never recorded.
   */
  void refreshStakeholders();
  /**
   * Block path (seal / validate / replay): install the stake snapshot for the
   * block's epoch. The first block of an epoch records it from the tip bank,
   * so every node derives the same snapshot regardless of refresh timing.
   */
  void refreshStakeholders(uint64_t blockSlot);

  /**
   * Collect up to kEpochSeedLookback block hashes from `prevEpoch`
   * (oldest→newest) for epoch-seed tip material.
   */
  std::vector<std::string> collectPrevEpochLookbackHashes(uint64_t prevEpoch) const;

  /** The newest stored block seed of an epoch ≤ `maxEpoch`: {epoch, seed}. */
  Roe<std::pair<uint64_t, std::string>> readLatestEpochSeed(uint64_t maxEpoch) const;

  /** Seed of `epoch` from the seed of `epoch - 1` (WIRE_SCHEMA § Epoch seed). */
  std::string deriveNextEpochSeed(uint64_t epoch, const std::string &prevSeed,
                                  const std::string &stakeHash) const;

protected:
  /** Block `index` of `epoch` becomes the next checkpoint (chain_block::rotatesCheckpoint). */
  bool rotatesCheckpointAt(uint64_t index, uint64_t epoch) const;

private:
  /** Maximum blocks to scan in findTransactionsByWalletId to avoid long runs.
   */
  constexpr static const uint64_t MAX_BLOCKS_TO_SCAN_FOR_WALLET_TX = 32;
  constexpr static const uint64_t THRESHOLD_TXES_FOR_WALLET_TX = 32;

  chain_block::BlockAdmissionMode admissionModeFor(uint64_t blockIndex) const;

  /** Account metadata for renewal: user accounts get genesis balance adjusted
   * to post-renewal (current - fee) since verifyBalance expects that. Uses
   * TxContext account-meta extractors (from RecordHandler). */
  Roe<std::string>
  getUpdatedAccountMetadataForRenewal(const Ledger::Block &block,
                                      const AccountBuffer::Account &account,
                                      uint64_t minFee) const;

  Roe<void>
  verifySignaturesAgainstAccount(const std::string &message,
                                 const std::vector<std::string> &signatures,
                                 const AccountBuffer::Account &account) const;


  /** Calculate the maximum blockId for account renewals at a given block and slot. */
  Roe<uint64_t> calculateMaxBlockIdForRenewal(uint64_t atBlockId, uint64_t atSlot) const;

  Roe<uint64_t> calculateMinimumFeeForAccountMeta(const AccountBuffer &bank,
                                                  uint64_t accountId) const;

  /** Create a renewal or end-user transaction for a given account. */
  Roe<Ledger::Record>
  createRenewalTx(uint64_t accountId, uint64_t slot) const;

  /** Tip state outside the bank that block admission may change. */
  struct TipSnapshot {
    consensus::SlotCommittee::TipState consensus;
    std::optional<BlockChainConfig> optChainConfig;
    std::optional<PendingChainConfig> pendingChainConfig;
    std::optional<IssuanceBaseline> issuanceBaseline;
    Checkpoint checkpoint;
    std::optional<ChainAuthority> optAuthority;
  };
  /** Open a bank overlay and snapshot the rest of the tip state. */
  TipSnapshot beginTipUpdate();
  void commitTipUpdate();
  void rollbackTipUpdate(TipSnapshot snapshot);

  Roe<void> applyNextBlock(const Ledger::ChainNode &block);
  /**
   * Before a block's records: stake snapshot and epoch seed for its epoch. A node that starts at a
   * checkpoint first installs config, consensus, authority and the genesis account from the
   * checkpoint block's genesis renewal (beacon-authority ba5b). Under CheckpointReplay the epoch
   * seed is the block's own (trusted catch-up); under Full it is derived.
   */
  Roe<void> prepareBlockEpoch(const Ledger::ChainNode &block, chain_block::BlockAdmissionMode mode);
  /** Record `epoch`'s committee (dropping later epochs' snapshots, which predate it). */
  void recordEpochStakeSnapshot(uint64_t epoch, std::vector<consensus::Stakeholder> stakeholders);
  /** A node starting at checkpoint `block`: install what its genesis renewal carries. */
  Roe<void> installCheckpointState(const Ledger::ChainNode &block);
  /**
   * Before a block of `epoch` is checked: put a pending config update in force
   * once its epoch has come, and on an epoch's first block record genesis's
   * balance as the issuance baseline.
   */
  void enterBlockEpoch(uint64_t epoch);
  /** Stake set for leader election from the tip bank: registered miners with at least
   *  minerMinStake (WIRE_SCHEMA § Miner registration and liveness). */
  std::vector<consensus::Stakeholder> electionCandidates() const;
  /** Block-level miner liveness after the records (chain_tx::applyMinerLiveness). */
  Roe<void> applyMinerLiveness(const Ledger::ChainNode &block, chain_block::BlockAdmissionMode mode);
  Roe<void> sealOnTip(Ledger::ChainNode &block);

  Roe<void> processBlock(const Ledger::ChainNode &block,
                         chain_block::BlockAdmissionMode mode);
  Roe<void> processGenesisBlock(const Ledger::ChainNode &block);
  Roe<void> processNormalBlock(const Ledger::ChainNode &block,
                               chain_block::BlockAdmissionMode mode);
  /**
   * Assemble stage: fill epoch / stakeSnapshotHash / epochSeed / txRoot.
   * Does not apply txs or set stateRoot/hash (those happen after Check/Apply).
   */
  Roe<void> assembleBlockHeader(Ledger::ChainNode &block);
  /** Persist a block whose effects were already applied by sealBlock. */
  Roe<void> commitSealedBlock(const Ledger::ChainNode &block);
  Roe<void> commitSealedOnTip(const Ledger::ChainNode &block);
  void maybeRotateCheckpoint(const Ledger::ChainNode &block);

  Roe<void> processGenesisTxRecord(
      const Ledger::Record &record);
  Roe<void>
  processNormalTxRecord(const Ledger::Record &record,
                        uint64_t blockId, uint64_t blockSlot,
                        uint64_t slotLeaderId,
                        chain_block::BlockAdmissionMode admissionMode);
  Roe<void>
  validateTxSignatures(const Ledger::Record &record,
                       uint64_t slotLeaderId,
                       chain_block::BlockAdmissionMode admissionMode) const;

  TxContext txContext_{};

  RecordHandler recordHandler_{};

  /** Tip bank already reflects this sealed-but-uncommitted block. */
  struct PendingSeal {
    uint64_t index{0};
    std::string hash;
  };
  std::optional<PendingSeal> pendingSeal_;
  /** Pre-seal tip state; the bank overlay stays open until commit/rollback. */
  std::optional<TipSnapshot> sealSnapshot_;
  /** Authority-changing records of committed blocks, from `authorityStepsFrom_` (0 = genesis). */
  std::vector<Client::AuthorityStep> authoritySteps_;
  uint64_t authorityStepsFrom_{0};
  /** Index a committed block's authority-changing records. */
  void noteAuthoritySteps(const Ledger::ChainNode &block);
  /** Highest certificate seen out of band and not committed yet (ba5c), with its record. */
  std::optional<BeaconCertificate> seenCert_;
  std::optional<Ledger::Record> seenCertRecord_;
  /** After the authority changed: drop the seen certificate once committed or no longer signed. */
  void refreshSeenCertificate();

  /** Stake snapshot per epoch, recorded by the block path only (see refreshStakeholders). */
  std::map<uint64_t, std::vector<consensus::Stakeholder>> epochStakeSnapshots_;
  /** Recorded epochs kept behind the newest one processed. */
  static constexpr uint64_t kStakeSnapshotsKept = 4;
};

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config);
std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config);

} // namespace pp

#endif // PP_LEDGER_CHAIN_H
