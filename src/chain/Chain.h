#ifndef PP_LEDGER_CHAIN_H
#define PP_LEDGER_CHAIN_H

#include "ledger/client/Client.h"
#include "ledger/consensus/SlotCommittee.h"
#include "ledger/ledger/Ledger.h"
#include "ledger/chain/AccountBuffer.h"
#include "chain/BlockValidation.h"
#include "ledger/chain/ErrorCodes.h"
#include "ledger/chain/RecordHandler.h"
#include "ledger/chain/TxContext.h"
#include "ledger/chain/TxError.h"
#include "ledger/chain/Types.h"
#include "chain/StateSnapshot.h"
#include "ledger/chain/AuthorityTrail.h"
#include "ledger/lib/common/Crypto.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "ledger/lib/common/Utilities.h"

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
  /**
   * What a pool checks before it holds `record` (load-resilience lr4), cheap against the committed
   * state: it decodes, its validity window has not ended at `nowSec`, its payer can cover the amount
   * and the fee, and — when `verifySignatures` — its signer's keys signed it. What depends on the
   * rest of a block (pending spends, fee rules on metadata, idempotent ids) is the leader's to check.
   */
  Roe<void> checkAdmission(const Ledger::Record &record, int64_t nowSec, bool verifySignatures) const;
  /** (payer, idempotent id) of a record that has one (RecordHandler::getIdempotencyKey). */
  std::optional<std::pair<uint64_t, uint64_t>> idempotencyKeyOf(const Ledger::Record &record) const;
  /** Max transactions per block (0 = no limit), renewals included (B035). */
  uint64_t getMaxTransactionsPerBlock() const;
  /** Max bytes per block (0 = no limit; B034). */
  uint64_t getMaxBlockBytes() const;
  /**
   * Bytes the next block at `slot` takes before its records: the header, and the committee when it
   * opens an epoch (estimated from today's stakeholders, with room for 64 more). For a leader's
   * budget; the seal checks the real size.
   */
  uint64_t estimateBlockShellBytes(uint64_t slot) const;
  /**
   * Empty heartbeat lag in slots from genesis config (`0` = empty seals off).
   */
  uint64_t getHeartbeatSlots() const;
  /** networkId from loaded chain config (empty if config not ready). */
  std::string getNetworkId() const;
  /** Recovery set + beacon certificate in force; nullopt before genesis is applied. */
  std::optional<ChainAuthority> getAuthority() const { return txContext_.optAuthority; }
  /** The config in force at the tip. */
  std::optional<BlockChainConfig> getChainConfig() const { return txContext_.optChainConfig; }
  /**
   * The genesis meta in force for a block of the current epoch: the config in force and any update
   * pending, the admin account (keys, threshold), the authority — what CONFIG_GET serves, and what
   * a genesis renewal carries.
   */
  Roe<GenesisAccountMeta> genesisMetaInForce() const;
  /**
   * Every account of the state at the tip, by ascending id: its leaf fields and its attachment
   * (read from the record that last wrote it). For a re-genesis export (StateSnapshot, bp8);
   * checks each attachment against the hash the account holds.
   */
  Roe<std::vector<StateSnapshot::Account>> exportAccounts() const;
  /** The recovery pending on this chain (beacon-authority ba7), as STATUS shows it; none if none. */
  std::optional<Client::RecoveryStatus> recoveryStatus() const;
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
   * The rewind the held certificate orders (beacon-authority bp1, B023): this chain holds blocks
   * above its cutover — signed by a superseded generation past it — so they must go. None when it
   * holds none, or holds no certificate out of band.
   */
  std::optional<Client::RewindReport> rewindOrder() const;
  /**
   * How far back `order` reaches, in slot time: from the cutover block — or the first block this
   * chain holds, when the cutover lies before it — to the block it would drop last. None when those
   * blocks cannot be read.
   */
  std::optional<uint64_t> rewindDepthSeconds(const Client::RewindReport &order) const;
  /**
   * Whether `order` would drop a final block (projects/finality FN003): its cutover lies below the
   * finalized height. A node does that only with its operator's approval. None when the blocks
   * cannot be read (treated as crossing).
   */
  std::optional<bool> rewindCrossesFinality(const Client::RewindReport &order) const;

  /** The finality bound in force at the tip, in seconds (FN001); 0 before a config is loaded. */
  uint64_t getFinalitySeconds() const;
  /** The bound blocks of `epoch` were committed under (FN002): the history's entry for it. */
  uint64_t finalitySecondsAt(uint64_t epoch) const;
  /**
   * The highest block such that it and every block below it held here are past their finality
   * times — slot start plus the bound they were committed under — by this node's clock (FN002).
   * None when no block held is final yet; an error when a block cannot be read.
   */
  Roe<std::optional<uint64_t>> finalizedHeight() const;
  /**
   * The held certificate vouches for a block this chain holds at its cutover height, but with
   * another hash (bp2): history forked at or below it, or the certificate names the wrong block.
   * The node does not rewind then — a human must look.
   */
  bool cutoverForked() const;
  /**
   * The block a held certificate vouches for (`cutoverHeight`, `cutoverHash`): a node behind it may
   * sync up to it with no signed tip (BlockSync). None without a held certificate with a cutover.
   */
  std::optional<std::pair<uint64_t, std::string>> vouchedCutover() const;
  /**
   * The certificate block `blockId` must commit (bp3): the one held out of band, when it has a
   * cutover, `blockId` is the block after it and this chain holds the block it vouches for. The
   * slot leader puts it in its block itself — the beacon taking over commits nothing else there.
   */
  std::optional<Ledger::Record> certificateDueAt(uint64_t blockId) const;
  /** Recalibrated to the beacon's clock (consensus::SlotCommittee::setTimeOffsetMs). */
  void setTimeOffsetMs(int64_t offsetMs) { txContext_.consensus.setTimeOffsetMs(offsetMs); }
  /**
   * Whether an upstream serving `tip` is worth following (beacon-authority bp2b, ranking): not of a
   * generation older than the one in force, not past the cutover of a held certificate, not above
   * equivocation evidence held, not of a generation no certificate here names (offer the
   * upstream's certificate first). A cheap pre-check — the sync verifies the tip itself.
   */
  bool mayFollowTip(const Client::SignedTip &tip) const;
  /**
   * Where to reach the beacon (bp2b): the held certificate's `dialHints`, then those of the one in
   * force; deduplicated, newest first.
   */
  std::vector<std::string> dialHints() const;
  /**
   * The current certificate, as STATUS serves it: the one held out of band, else the latest
   * committed T_BEACON_CERT this node holds (none at generation 1, or from before a checkpoint
   * start).
   */
  std::optional<Ledger::Record> currentCertificateRecord() const;
  /** What offerEquivocation did with equivocation evidence (beacon-authority ba6). */
  enum class EquivocationOffer {
    Newer,   ///< verified and kept: this node halts above its height
    Same,    ///< already held
    Ignored, ///< superseded generation (a later certificate is in force or seen), or evidence for
             ///< the generation held already
  };
  /**
   * Equivocation evidence from a peer, a file or this node's own tips (beacon-authority ba6):
   * both tips signed by the key of their generation — the one in force, or the certificate held
   * out of band. Once kept, tips of that generation above its height do not verify
   * (authorityTrail()): the node commits and produces nothing past it. Dropped when a certificate
   * of a later generation is seen or committed (resume). In memory only: the caller persists it.
   */
  Roe<EquivocationOffer> offerEquivocation(const Client::Equivocation &evidence);
  /** The evidence held; none unless this node is halted on it. */
  const std::optional<Client::Equivocation> &equivocation() const { return equivocation_; }

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
  /**
   * Who produced blocks [from, to) (block 0 not counted), the fees in them, and the misses charged
   * to due miners (WIRE_SCHEMA § Producer statistics); at most Client::kMaxProducerStatsBlocks.
   * Refused for blocks this node does not hold.
   */
  Roe<Client::ProducerStatsResponse> producerStats(uint64_t fromBlockId, uint64_t toBlockId) const;
  /**
   * Account `accountId` (or its absence) in the tip block's state root, with its Merkle path
   * (WIRE_SCHEMA § Account proofs). Refused while the account state is ahead of the tip (a block
   * being sealed): ask again.
   */
  Roe<Client::AccountProof> accountProof(uint64_t accountId) const;
  /** `domain`'s binding (or absence) in the tip block's state root, with its path (projects/domains). */
  Roe<Client::DomainProof> domainProof(const std::string &domain) const;
  /** Who `domain` is bound to in the committed state, if anyone. */
  std::optional<AccountBuffer::DomainEntry> findDomain(const std::string &domain) const;

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
  /**
   * Install or derive SlotCommittee epoch seed for `epoch` (no-op if already set), from the blocks
   * below `endBlockId` (default: all the ledger holds).
   */
  Roe<void> ensureEpochSeed(uint64_t epoch, std::optional<uint64_t> endBlockId = std::nullopt);
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
  /** When added blocks become durable (Ledger::CommitPolicy); the beacon commits every block. */
  void setLedgerCommitPolicy(const Ledger::CommitPolicy &policy) { txContext_.ledger.setCommitPolicy(policy); }
  Roe<uint64_t> loadFromLedger(uint64_t startingBlockId);
  /** Ledger::getBlockReadCount of this chain's ledger. */
  uint64_t getLedgerBlockReadCount() const { return txContext_.ledger.getBlockReadCount(); }
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
   * (oldest→newest) for epoch-seed tip material, among the blocks below `endBlockId`.
   *
   * The scans run backwards from `endBlockId`: when the ledger holds blocks past the one being
   * checked (a node replaying its ledger at start), they skip them without reading — from the
   * ledger's tip, a replay read the rest of the chain for every epoch (load-resilience lr9).
   */
  std::vector<std::string> collectPrevEpochLookbackHashes(uint64_t prevEpoch, uint64_t endBlockId) const;

  /** The newest stored block seed of an epoch ≤ `maxEpoch` below `endBlockId`: {epoch, seed}. */
  Roe<std::pair<uint64_t, std::string>> readLatestEpochSeed(uint64_t maxEpoch, uint64_t endBlockId) const;

  /** Seed of `epoch` from the seed of `epoch - 1` (WIRE_SCHEMA § Epoch seed), from the blocks below `endBlockId`. */
  std::string deriveNextEpochSeed(uint64_t epoch, const std::string &prevSeed, const std::string &stakeHash,
                                  uint64_t endBlockId) const;

protected:
  /** Block `index` of `epoch` may become the next checkpoint (chain_block::checkpointEligible). */
  bool checkpointEligibleAt(uint64_t index, uint64_t epoch) const;

private:
  /** Set `gm`'s config state, authority and admin set to those in force for a block of `epoch`. */
  void setStateInForce(GenesisAccountMeta &gm, uint64_t epoch) const;
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
    std::optional<ImportProgress> importProgress;
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
  /** A pending recovery whose epoch has come replaces the admin set, in place (ba7). */
  void applyDueRecovery(uint64_t epoch);
  /** The last epoch whose pending recovery was announced in the log (one alert per epoch). */
  std::optional<uint64_t> recoveryNoticeEpoch_;
  /** Stake set for leader election from the tip bank: registered miners with at least
   *  minerMinStake (WIRE_SCHEMA § Miner registration and liveness). */
  std::vector<consensus::Stakeholder> electionCandidates() const;
  /** Block-level miner liveness after the records (chain_tx::applyMinerLiveness). */
  /** Account recoveries due by the block's epoch, applied at an epoch's first block (projects/account-recovery). */
  void applyDueAccountRecoveries(const Ledger::ChainNode &block);
  Roe<void> applyMinerLiveness(const Ledger::ChainNode &block, chain_block::BlockAdmissionMode mode);
  Roe<void> sealOnTip(Ledger::ChainNode &block);

  Roe<void> processBlock(const Ledger::ChainNode &block,
                         chain_block::BlockAdmissionMode mode);
  Roe<void> processGenesisBlock(const Ledger::ChainNode &block);
  /** True for a block in an importing genesis's span (B032): after block 0, until the last import. */
  bool isImportBlock(const Ledger::ChainNode &block) const;
  /** An import block: its own rules (no slot, leader or body policy), its imports, its state root. */
  Roe<void> processImportBlock(const Ledger::ChainNode &block);
  /** After an import block applied: once every account is in, the old leaves must hash to the
   *  predecessor's state root; then the span ends. */
  Roe<void> finishImportBlock();
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
  /**
   * The finality bound by epoch, as blocks committed here saw it (FN002): (first epoch, seconds),
   * ascending; a new entry whenever a committed block's epoch runs under another bound. Rebuilt by
   * every load (blocks are replayed); before the first block held, the first entry stands.
   */
  std::vector<std::pair<uint64_t, uint64_t>> finalityBounds_;
  /** Note the bound `block` (committed) ran under. */
  void noteFinalityBound(const Ledger::ChainNode &block);
  /** Highest certificate seen out of band and not committed yet (ba5c), with its record. */
  std::optional<BeaconCertificate> seenCert_;
  std::optional<Ledger::Record> seenCertRecord_;
  /** After the authority changed: drop the seen certificate once committed or no longer signed. */
  void refreshSeenCertificate();
  /** Equivocation evidence held (ba6), not superseded by a later certificate. */
  std::optional<Client::Equivocation> equivocation_;
  /** True once a certificate above `generation` is in force or seen out of band. */
  bool isGenerationSuperseded(uint64_t generation) const;
  /** Drop held evidence whose generation a later certificate superseded (resume). */
  void refreshEquivocation();

  /**
   * Each block's misses charged (miner liveness): the leaders, in slot order, of the slots it skipped
   * while due. Written for every block applied (none: erased), so a block sealed and abandoned, or
   * dropped at a cutover, is replaced by the block applied at its index; read below the tip only.
   * Rebuilt by the replay at start.
   */
  std::map<uint64_t, std::vector<uint64_t>> chargedMisses_;

  /** Stake snapshot per epoch, recorded by the block path only (see refreshStakeholders). */
  std::map<uint64_t, std::vector<consensus::Stakeholder>> epochStakeSnapshots_;
  /** Recorded epochs kept behind the newest one processed. */
  static constexpr uint64_t kStakeSnapshotsKept = 4;
};

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config);
std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config);

} // namespace pp

#endif // PP_LEDGER_CHAIN_H
