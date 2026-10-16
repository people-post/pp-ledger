#include "EquivocationStore.h"
#include "NodeFile.h"

namespace pp {

EquivocationStore::EquivocationStore(Chain &chain, std::string path) : chain_(chain), path_(std::move(path)) {
  redirectLogger("EquivocationStore");
}

EquivocationStore::Roe<void> EquivocationStore::load() {
  pending_.reset();
  auto read = node_file::readIfExists(path_);
  if (!read) {
    return Error(E_IO, read.error().message);
  }
  if (!read.value()) {
    return {};
  }
  stored_ = true;
  Client::Equivocation evidence;
  if (!evidence.ltsFromString(*read.value())) {
    return Error(E_IO, path_ + " is not equivocation evidence");
  }
  if (!chain_.getAuthority().has_value()) {
    log().info << "Holding the stored equivocation evidence until the chain has its first block";
    pending_ = std::move(evidence);
    return {};
  }
  offerStored(evidence);
  return {};
}

void EquivocationStore::retryPending() {
  if (!pending_ || !chain_.getAuthority().has_value()) {
    return;
  }
  const Client::Equivocation evidence = std::move(*pending_);
  pending_.reset();
  offerStored(evidence);
}

void EquivocationStore::offerStored(const Client::Equivocation &evidence) {
  auto offered = chain_.offerEquivocation(evidence);
  if (!offered) {
    log().warning << "Dropping the stored equivocation evidence: " << offered.error().message;
    remove();
  } else if (offered.value() == Chain::EquivocationOffer::Ignored) {
    log().info << "The stored equivocation evidence is superseded by a later beacon generation: removing it";
    remove();
  }
}

EquivocationStore::Roe<Chain::EquivocationOffer> EquivocationStore::offer(const Client::Equivocation &evidence) {
  auto offered = chain_.offerEquivocation(evidence);
  if (!offered) {
    return Error(offered.error().code, offered.error().message);
  }
  if (offered.value() == Chain::EquivocationOffer::Newer) {
    pending_.reset();
    if (auto written = node_file::replace(path_, evidence.ltsToString()); !written) {
      // Kept in memory: the node stays halted until a restart; say so loudly.
      log().error << "Holding equivocation evidence in memory only: " << written.error().message;
    } else {
      stored_ = true;
    }
  }
  return offered.value();
}

std::optional<Chain::EquivocationOffer> EquivocationStore::note(const Client::Equivocation &evidence,
                                                                const std::string &from) {
  auto offered = offer(evidence);
  if (offered) {
    return offered.value();
  }
  if (const std::string bytes = evidence.ltsToString(); bytes != lastRefused_) {
    lastRefused_ = bytes;
    log().warning << "Equivocation evidence from " << from << " not kept: " << offered.error().message;
  }
  return std::nullopt;
}

void EquivocationStore::refresh() {
  if (stored_ && !pending_ && !chain_.equivocation().has_value()) {
    remove();
  }
}

void EquivocationStore::remove() {
  if (auto removed = node_file::remove(path_); !removed) {
    log().warning << removed.error().message;
    return;
  }
  stored_ = false;
}

} // namespace pp
