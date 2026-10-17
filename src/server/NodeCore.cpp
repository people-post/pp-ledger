#include "NodeCore.h"
#include "common/Logger.h"

namespace pp {

NodeCore::NodeCore(const std::string &name) {
  redirectLogger(name);
  chain_.redirectLogger(log().getFullName() + ".Chain");
}

NodeCore::Roe<Client::UserAccount> NodeCore::getAccount(uint64_t accountId) const {
  auto result = chain_.getAccount(accountId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

NodeCore::Roe<Ledger::ChainNode> NodeCore::readBlock(uint64_t blockId) const {
  auto result = chain_.readBlock(blockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

std::optional<std::string> NodeCore::getTipHash() const {
  const uint64_t next = getNextBlockId();
  if (next == 0) {
    return std::nullopt;
  }
  auto tip = readBlock(next - 1);
  return tip ? std::optional<std::string>(tip.value().hash) : std::nullopt;
}

NodeCore::Roe<Client::AuthorityProof> NodeCore::authorityProof(uint64_t upToBlockId) const {
  auto proof = chain_.authorityProof(upToBlockId);
  if (!proof) {
    return Error(proof.error().code, proof.error().message);
  }
  return proof.value();
}

NodeCore::Roe<std::vector<Ledger::Record>> NodeCore::findTransactionsByWalletId(uint64_t walletId,
                                                                                uint64_t &ioBlockId) const {
  auto result = chain_.findTransactionsByWalletId(walletId, ioBlockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

NodeCore::Roe<Ledger::Record> NodeCore::findTransactionByIndex(uint64_t txIndex) const {
  auto result = chain_.findTransactionByIndex(txIndex);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

NodeCore::Roe<void> NodeCore::checkAdmission(const Ledger::Record &record, int64_t nowSec,
                                             bool verifySignatures) const {
  auto checked = chain_.checkAdmission(record, nowSec, verifySignatures);
  if (!checked) {
    return Error(checked.error().code, checked.error().message);
  }
  return {};
}

uint64_t NodeCore::getGeneration() const {
  const auto authority = chain_.getAuthority();
  return authority ? authority->beacon.generation : 0;
}

NodeCore::Roe<void> NodeCore::applyPendingRewind() {
  auto applied = rewind::applyPendingOrder(workDir_, ledgerDir());
  if (!applied) {
    return Error(E_OPEN, "Failed to apply the pending rewind: " + applied.error().message);
  }
  if (const auto &order = applied.value()) {
    log().error << "Rewound: kept blocks up to " << order->keptUpTo << ", dropped " << order->keptUpTo + 1
                << " to " << order->droppedUpTo << " (past the cutover of beacon generation " << order->generation
                << ")";
    if (auto handled = onRewound(order->keptUpTo); !handled) {
      return handled;
    }
  }
  auto last = rewind::lastApplied(workDir_);
  if (!last) {
    return Error(E_OPEN, last.error().message);
  }
  lastRewind_ = last.value();
  return {};
}

NodeCore::Roe<void> NodeCore::onRewound(uint64_t /*keptUpTo*/) { return {}; }

NodeCore::Roe<bool> NodeCore::scheduleRewind(uint64_t approvedGeneration) {
  auto scheduled = rewind::schedule(chain_, workDir_, approvedGeneration, maxAutomaticRewind_);
  if (!scheduled) {
    return Error(E_OPEN, "Failed to write the rewind order: " + scheduled.error().message);
  }
  const auto &order = scheduled.value().order;
  switch (scheduled.value().kind) {
  case rewind::Scheduled::Kind::None:
    return false;
  case rewind::Scheduled::Kind::HeldBack:
    if (heldBackGeneration_ != order->generation) {
      heldBackGeneration_ = order->generation;
      const auto &depth = scheduled.value().depthSeconds;
      log().error << "Not rewinding on its own: beacon generation " << order->generation << " orders a cut to block "
                  << order->keptUpTo << ", "
                  << (depth ? std::to_string(*depth / 3600) + " h" : std::string("an unknown time"))
                  << " of slots back — more than the " << maxAutomaticRewind_.count() / 3600
                  << " h a node rewinds unattended. Check the certificate; to apply it, restart with "
                  << "--approve-rewind " << order->generation;
    }
    return false;
  case rewind::Scheduled::Kind::Forked:
    if (heldBackGeneration_ != order->generation) {
      heldBackGeneration_ = order->generation;
      log().error << "Not rewinding: beacon generation " << order->generation << " vouches for another block "
                  << order->keptUpTo << " than this node holds — its history forked at or below it, or the "
                  << "certificate names the wrong block. Its tips past the cutover are refused; an operator "
                  << "must look";
    }
    return false;
  case rewind::Scheduled::Kind::Ordered:
    break;
  }
  log().error << "Rewinding to block " << order->keptUpTo << ": blocks " << order->keptUpTo + 1 << " to "
              << order->droppedUpTo << " are past the cutover of beacon generation " << order->generation
              << "; restarting";
  return true;
}

NodeCore::Roe<void> NodeCore::openStores() {
  certificates_ = std::make_unique<CertificateStore>(chain_, workDir_ + "/" + FILE_BEACON_CERT);
  certificates_->redirectLogger(log().getFullName() + ".CertificateStore");
  if (auto loaded = certificates_->load(); !loaded) {
    return Error(E_OPEN, loaded.error().message);
  }
  equivocations_ = std::make_unique<EquivocationStore>(chain_, workDir_ + "/" + FILE_EQUIVOCATION);
  equivocations_->redirectLogger(log().getFullName() + ".EquivocationStore");
  if (auto loaded = equivocations_->load(); !loaded) {
    return Error(E_OPEN, loaded.error().message);
  }
  return {};
}

void NodeCore::refreshStores() {
  if (certificates_) {
    certificates_->retryPending(); // one loaded before the chain could verify it
    certificates_->refresh();      // a block may have committed it, or replaced the admins who signed it
  }
  if (equivocations_) {
    equivocations_->retryPending();
    equivocations_->refresh(); // a certificate on chain may have resumed it
  }
}

} // namespace pp
