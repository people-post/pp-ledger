#ifndef PP_LEDGER_NODE_CORE_H
#define PP_LEDGER_NODE_CORE_H

#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "CertificateStore.h"
#include "Chain.h"
#include "EquivocationStore.h"
#include "Rewind.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pp {

/**
 * What every node role (Beacon, Relay, Miner) holds: the chain under its work directory, the
 * certificate and equivocation stores beside the ledger, the reads served from it, and the rewind
 * a certificate may order (beacon-authority bp1, B022). Roles add what is theirs: the beacon signs
 * tips and offers certificates; followers (FollowerCore) verify and note what upstreams send.
 */
class NodeCore : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  ~NodeCore() override = default;

  // ----------------- reads -------------------------------------
  Chain::Checkpoint getCheckpoint() const { return chain_.getCheckpoint(); }
  uint64_t getNextBlockId() const { return chain_.getNextBlockId(); }
  uint64_t getCurrentSlot() const { return chain_.getCurrentSlot(); }
  uint64_t getCurrentEpoch() const { return chain_.getCurrentEpoch(); }
  /** Slot duration in seconds. */
  uint64_t getSlotDuration() const { return chain_.getSlotDuration(); }
  std::vector<consensus::Stakeholder> getStakeholders() const { return chain_.getStakeholders(); }
  /** The network id of the chain held (empty before it loads). */
  std::string getNetworkId() const { return chain_.getNetworkId(); }
  std::string calculateHash(const Ledger::Block &block) const { return chain_.calculateHash(block); }

  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;
  /** The genesis meta in force (Chain::genesisMetaInForce). */
  Roe<GenesisAccountMeta> genesisMetaInForce() const;
  Roe<Ledger::ChainNode> readBlock(uint64_t blockId) const;
  /** Hash of the last block held; none while the ledger is empty. */
  std::optional<std::string> getTipHash() const;
  /** AUTHORITY_PROOF up to `upToBlockId` (Chain::authorityProof). */
  Roe<Client::AuthorityProof> authorityProof(uint64_t upToBlockId) const;
  /** Find transactions involving walletId, scanning backwards from ioBlockId (0 = latest). ioBlockId is updated to the last block scanned. */
  Roe<std::vector<Ledger::Record>> findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const;
  /** Chain::accountProof. */
  Roe<Client::AccountProof> accountProof(uint64_t accountId) const;
  Roe<Client::DomainProof> domainProof(const std::string &domain) const;
  /** Chain::producerStats. */
  Roe<Client::ProducerStatsResponse> producerStats(uint64_t fromBlockId, uint64_t toBlockId) const;
  /** Find transaction by global chain index (0-based). */
  Roe<Ledger::Record> findTransactionByIndex(uint64_t txIndex) const;

  // ----------------- authority -------------------------------------
  /** The beacon generation in force (0 before the chain is loaded). */
  uint64_t getGeneration() const;
  /** The current certificate for STATUS (Chain::currentCertificateRecord). */
  std::optional<Ledger::Record> getCurrentCertificate() const { return chain_.currentCertificateRecord(); }
  /** The certificate held out of band, above the generation in force (Chain::seenCertificate). */
  std::optional<BeaconCertificate> getSeenCertificate() const { return chain_.seenCertificate(); }
  /** Equivocation evidence this node halts on (beacon-authority ba6), for STATUS; none if none. */
  std::optional<Client::Equivocation> getEquivocation() const { return chain_.equivocation(); }
  /** A recovery pending on this chain (beacon-authority ba7), for STATUS; none if none. */
  std::optional<Client::RecoveryStatus> getRecoveryStatus() const { return chain_.recoveryStatus(); }
  /** Where to reach the beacon: the certificates' dial hints (Chain::dialHints). */
  std::vector<std::string> getDialHints() const { return chain_.dialHints(); }

  /** Max records per block (0 = no limit) and max bytes (0 = no limit), from the config in force. */
  uint64_t getMaxTransactionsPerBlock() const { return chain_.getMaxTransactionsPerBlock(); }
  uint64_t getMaxBlockBytes() const { return chain_.getMaxBlockBytes(); }
  /** `signatures` over `message` by account `accountId`'s keys (Chain::E_ACCOUNT_NOT_FOUND: no such account). */
  Roe<void> verifyAccountSignatures(uint64_t accountId, const std::string &message,
                                    const std::vector<std::string> &signatures) const;
  /** What a pool checks before it holds `record` (Chain::checkAdmission). */
  Roe<void> checkAdmission(const Ledger::Record &record, int64_t nowSec, bool verifySignatures) const;
  /** (payer, idempotent id) of a record that has one. */
  std::optional<std::pair<uint64_t, uint64_t>> idempotencyKeyOf(const Ledger::Record &record) const {
    return chain_.idempotencyKeyOf(record);
  }

  /** Recompute the stakeholders from the chain. */
  void refresh() { chain_.refreshStakeholders(); }

  // ----------------- rewind -------------------------------------
  /**
   * When the held certificate orders a rewind (Chain::rewindOrder), write the order for the next
   * start — unless it reaches back further than the node rewinds on its own and the operator has
   * not approved its generation (`approvedGeneration`, 0 = none; rewind::schedule). True when an
   * order was written: the caller restarts.
   */
  Roe<bool> scheduleRewind(uint64_t approvedGeneration);
  /** Tests: how far back the node rewinds on its own (default rewind::kMaxAutomaticRewind). */
  void setMaxAutomaticRewind(std::chrono::seconds reach) { maxAutomaticRewind_ = reach; }
  /** The last rewind this node applied (STATUS); none if none. */
  const std::optional<Client::RewindReport> &getLastRewind() const { return lastRewind_; }

protected:
  /** `name`: this node's logger (the chain logs under it). */
  explicit NodeCore(const std::string &name);

  /** Error code of a failure while opening the node (mount, rewind, stores). */
  static constexpr int32_t E_OPEN = 2;

  constexpr static const char *DIR_LEDGER = "ledger";

  /** The work directory every file of the node lives under (set before applyPendingRewind). */
  void setWorkDir(std::string workDir) { workDir_ = std::move(workDir); }
  const std::string &workDir() const { return workDir_; }
  std::string ledgerDir() const { return workDir_ + "/" + DIR_LEDGER; }

  /**
   * At start, before the ledger mounts: apply a rewind ordered before the restart and load the last
   * one applied. Calls onRewound for an order applied now.
   */
  Roe<void> applyPendingRewind();
  /** A rewind was applied at this start, keeping blocks up to `keptUpTo`. */
  virtual Roe<void> onRewound(uint64_t keptUpTo);

  /** Once the chain is loaded: open the certificate and equivocation stores beside the ledger. */
  Roe<void> openStores();
  /** After a block commits: offer what waited for the chain, and refresh what the block may have changed. */
  void refreshStores();

  Chain chain_;
  std::unique_ptr<CertificateStore> certificates_;
  std::unique_ptr<EquivocationStore> equivocations_;

private:
  /** The highest beacon certificate seen out of band, beside the ledger. */
  constexpr static const char *FILE_BEACON_CERT = "beacon-cert.dat";
  /** Equivocation evidence this node halts on (ba6), beside the ledger. */
  constexpr static const char *FILE_EQUIVOCATION = "equivocation.dat";

  std::string workDir_;
  std::optional<Client::RewindReport> lastRewind_;
  std::chrono::seconds maxAutomaticRewind_{rewind::kMaxAutomaticRewind};
  /** Generation whose rewind was last reported as held back (one alert per certificate). */
  uint64_t heldBackGeneration_{0};
};

} // namespace pp

#endif // PP_LEDGER_NODE_CORE_H
