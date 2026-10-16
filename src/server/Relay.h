#ifndef PP_LEDGER_RELAY_H
#define PP_LEDGER_RELAY_H

#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "CertificateStore.h"
#include "Rewind.h"
#include "EquivocationStore.h"
#include "Chain.h"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pp {

/**
 * Relay - Core consensus and ledger management
 *
 * Responsibilities:
 * - Maintain full blockchain history from genesis
 * - Serve as authoritative data source for the network
 * - Coordinate with RelayServer for network communication
 */
class Relay : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  struct InitConfig {
    std::string workDir;
    int64_t timeOffset{0};
    uint64_t startingBlockId{0};
  };

  Relay();
  ~Relay() override = default;

  // ----------------- accessors -------------------------------------
  Chain::Checkpoint getCheckpoint() const;
  uint64_t getNextBlockId() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  /** Slot duration in seconds (for sync rate limiting). */
  uint64_t getSlotDuration() const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;

  Roe<Ledger::ChainNode> readBlock(uint64_t blockId) const;
  /** AUTHORITY_PROOF up to `upToBlockId` (Chain::authorityProof). */
  Roe<Client::AuthorityProof> authorityProof(uint64_t upToBlockId) const;
  std::string calculateHash(const Ledger::Block &block) const;
  /** A beacon signed tip, of the generation in force here or after `pending` (Chain::verifySignedTip). */
  Roe<void> verifySignedTip(const Client::SignedTip &tip, const std::vector<const Ledger::Block *> &pending = {}) const;
  /** The beacon authority at the local tip (Chain::authorityTrail). */
  Roe<AuthorityTrail> authorityTrail() const;
  /** Hash of the last block held; none while the ledger is empty. */
  std::optional<std::string> getTipHash() const;
  /** The current certificate for STATUS (Chain::currentCertificateRecord). */
  std::optional<Ledger::Record> getCurrentCertificate() const;
  /** Equivocation evidence this node halts on (beacon-authority ba6), for STATUS; none if none. */
  std::optional<Client::Equivocation> getEquivocation() const { return chain_.equivocation(); }
  /** A recovery pending on this chain (beacon-authority ba7), for STATUS; none if none. */
  std::optional<Client::RecoveryStatus> getRecoveryStatus() const { return chain_.recoveryStatus(); }
  /** Find transactions involving walletId, scanning backwards from ioBlockId (0 = latest). ioBlockId is updated to the last block scanned. */
  Roe<std::vector<Ledger::Record>>
  findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const;
  /** Find transaction by global chain index (0-based). */
  Roe<Ledger::Record>
  findTransactionByIndex(uint64_t txIndex) const;

  // ----------------- methods -------------------------------------
  Roe<void> init(const InitConfig &config);
  void refresh();

  Roe<void> addBlock(const Ledger::ChainNode &block);

  /**
   * At start, before the ledger mounts: apply a rewind ordered before the restart
   * (beacon-authority bp1, B022) and load the last one applied.
   */
  Roe<void> applyPendingRewind(const std::string &ledgerDir);
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

  /**
   * A beacon certificate from outside the blocks (ba5c: STATUS, TX_ADD), kept and persisted if it
   * is the highest seen (CertificateStore::note). None when refused (logged).
   */
  std::optional<Chain::CertificateOffer> noteCertificate(const Ledger::Record &record, const std::string &from);
  /**
   * Equivocation evidence (ba6: found in tips, STATUS, EQUIVOCATION_ADD), kept and persisted when it
   * verifies (EquivocationStore::note): the node then commits nothing of that generation above its
   * height. None when refused (logged).
   */
  std::optional<Chain::EquivocationOffer> noteEquivocation(const Client::Equivocation &evidence,
                                                           const std::string &from);
  /** The certificate held out of band, above the generation in force (Chain::seenCertificate). */
  std::optional<BeaconCertificate> getSeenCertificate() const { return chain_.seenCertificate(); }

private:
  constexpr static const char *DIR_LEDGER = "ledger";
  /** The highest beacon certificate seen out of band, beside the ledger. */
  constexpr static const char *FILE_BEACON_CERT = "beacon-cert.dat";
  /** Equivocation evidence this node halts on (ba6), beside the ledger. */
  constexpr static const char *FILE_EQUIVOCATION = "equivocation.dat";

  struct Config {
    std::string workDir;
    int64_t timeOffset{0};
  };

  Chain chain_;
  Config config_;
  std::unique_ptr<CertificateStore> certificates_;
  std::optional<Client::RewindReport> lastRewind_;
  std::chrono::seconds maxAutomaticRewind_{rewind::kMaxAutomaticRewind};
  /** Generation whose rewind was last reported as held back (one alert per certificate). */
  uint64_t heldBackGeneration_{0};
  std::unique_ptr<EquivocationStore> equivocations_;
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Relay::InitConfig &config);

} // namespace pp

#endif // PP_LEDGER_RELAY_H
