#ifndef PP_LEDGER_RELAY_H
#define PP_LEDGER_RELAY_H

#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "CertificateStore.h"
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
   * A beacon certificate from outside the blocks (ba5c: STATUS, TX_ADD), kept and persisted if it
   * is the highest seen (CertificateStore::note). None when refused (logged).
   */
  std::optional<Chain::CertificateOffer> noteCertificate(const Ledger::Record &record, const std::string &from);
  /** The certificate held out of band, above the generation in force (Chain::seenCertificate). */
  std::optional<BeaconCertificate> getSeenCertificate() const { return chain_.seenCertificate(); }

private:
  constexpr static const char *DIR_LEDGER = "ledger";
  /** The highest beacon certificate seen out of band, beside the ledger. */
  constexpr static const char *FILE_BEACON_CERT = "beacon-cert.dat";

  struct Config {
    std::string workDir;
    int64_t timeOffset{0};
  };

  Chain chain_;
  Config config_;
  std::unique_ptr<CertificateStore> certificates_;
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Relay::InitConfig &config);

} // namespace pp

#endif // PP_LEDGER_RELAY_H
