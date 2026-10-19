#include "server/FollowerCore.h"
#include "common/Logger.h"

#include <filesystem>

namespace pp {

FollowerCore::Roe<void> FollowerCore::addBlock(const Ledger::ChainNode &block) {
  auto result = chain_.addBlock(block);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  refreshStores();
  return {};
}

FollowerCore::Roe<void> FollowerCore::verifySignedTip(const Client::SignedTip &tip,
                                                      const std::vector<const Ledger::Block *> &pending) const {
  auto verified = chain_.verifySignedTip(tip, pending);
  if (!verified) {
    return Error(verified.error().code, verified.error().message);
  }
  return {};
}

FollowerCore::Roe<AuthorityTrail> FollowerCore::authorityTrail() const {
  auto trail = chain_.authorityTrail();
  if (!trail) {
    return Error(trail.error().code, trail.error().message);
  }
  return trail.value();
}

std::optional<Chain::CertificateOffer> FollowerCore::noteCertificate(const Ledger::Record &record,
                                                                     const std::string &from) {
  if (!certificates_) {
    return std::nullopt; // no chain yet: nothing to verify it against
  }
  auto offered = certificates_->note(record, from);
  if (offered == Chain::CertificateOffer::Newer && equivocations_) {
    equivocations_->refresh(); // a later generation resumes a halted one
  }
  return offered;
}

std::optional<Chain::EquivocationOffer> FollowerCore::noteEquivocation(const Client::Equivocation &evidence,
                                                                       const std::string &from) {
  if (!equivocations_) {
    return std::nullopt; // no chain yet: nothing to verify it against
  }
  return equivocations_->note(evidence, from);
}

FollowerCore::Roe<void> FollowerCore::openLedger(const std::string &workDir, int64_t timeOffsetMs,
                                                 uint64_t startingBlockId) {
  setWorkDir(workDir);
  if (!std::filesystem::exists(workDir)) {
    std::filesystem::create_directories(workDir);
  }
  log().info << "  Work directory: " << workDir;

  // Consensus first (the beacon's clock only; the rest of its config comes from the chain).
  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.timeOffsetMs = timeOffsetMs;
  chain_.initConsensus(consensusConfig);

  if (auto rewound = applyPendingRewind(); !rewound) {
    return rewound;
  }
  const std::string dir = ledgerDir();
  if (std::filesystem::exists(dir)) {
    auto mounted = chain_.mountLedger(dir);
    if (!mounted) {
      return Error(E_OPEN, "Failed to mount ledger: " + mounted.error().message);
    }
    if (getNextBlockId() < startingBlockId) {
      log().info << "Ledger data too old, removing existing work directory: " << dir;
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
      if (ec) {
        return Error(E_OPEN, "Failed to remove existing work directory: " + ec.message());
      }
    }
  }
  if (!std::filesystem::exists(dir)) {
    Ledger::InitConfig ledgerConfig;
    ledgerConfig.workDir = dir;
    ledgerConfig.startingBlockId = startingBlockId;
    if (auto created = chain_.initLedger(ledgerConfig); !created) {
      return Error(E_OPEN, "Failed to initialize ledger: " + created.error().message);
    }
  }
  if (auto loaded = chain_.loadFromLedger(startingBlockId); !loaded) {
    return Error(E_OPEN, "Failed to load from ledger: " + loaded.error().message);
  }
  return openStores();
}

} // namespace pp
