#include "CertificateStore.h"
#include "NodeFile.h"
#include "lib/common/BinaryPack.hpp"

namespace pp {

CertificateStore::CertificateStore(Chain &chain, std::string path) : chain_(chain), path_(std::move(path)) {
  redirectLogger("CertificateStore");
}

CertificateStore::Roe<void> CertificateStore::load() {
  pending_.reset();
  auto read = node_file::readIfExists(path_);
  if (!read) {
    return Error(E_IO, read.error().message);
  }
  if (!read.value()) {
    return {};
  }
  stored_ = true;
  const std::string bytes = *read.value();
  auto record = utl::binaryUnpack<Ledger::Record>(bytes);
  if (!record || record.value().type != Ledger::T_BEACON_CERT) {
    return Error(E_IO, path_ + " is not a beacon certificate record");
  }
  if (!chain_.getAuthority().has_value()) {
    log().info << "Holding the stored beacon certificate until the chain has its first block";
    pending_ = std::move(record.value());
    return {};
  }
  offerStored(record.value());
  return {};
}

void CertificateStore::retryPending() {
  if (!pending_ || !chain_.getAuthority().has_value()) {
    return;
  }
  const Ledger::Record record = std::move(*pending_);
  pending_.reset();
  offerStored(record);
}

void CertificateStore::offerStored(const Ledger::Record &record) {
  auto offered = chain_.offerCertificate(record);
  if (!offered) {
    log().warning << "Dropping the stored beacon certificate: " << offered.error().message;
    remove();
  } else if (offered.value() == Chain::CertificateOffer::Older) {
    log().info << "The stored beacon certificate is on chain now (or superseded): removing it";
    remove();
  }
}

CertificateStore::Roe<Chain::CertificateOffer> CertificateStore::offer(const Ledger::Record &record) {
  auto offered = chain_.offerCertificate(record);
  if (!offered) {
    return Error(offered.error().code, offered.error().message);
  }
  if (offered.value() == Chain::CertificateOffer::Newer) {
    pending_.reset();
    if (auto stored = persist(record); !stored) {
      // Kept in memory: the node refuses what it should until a restart; say so loudly.
      log().error << "Holding beacon certificate generation " << chain_.seenCertificate()->generation
                  << " in memory only: " << stored.error().message;
    }
  }
  return offered.value();
}

void CertificateStore::refresh() {
  if (stored_ && !pending_ && !chain_.seenCertificate().has_value()) {
    log().info << "The chain no longer holds the stored beacon certificate: removing it";
    remove();
  }
}

std::optional<Chain::CertificateOffer> CertificateStore::note(const Ledger::Record &record, const std::string &from) {
  auto offered = offer(record);
  if (offered) {
    return offered.value();
  }
  if (record.data != lastRefused_) {
    lastRefused_ = record.data;
    log().warning << "Beacon certificate from " << from << " not kept: " << offered.error().message;
  }
  return std::nullopt;
}

CertificateStore::Roe<void> CertificateStore::persist(const Ledger::Record &record) {
  if (auto written = node_file::replace(path_, utl::binaryPack(record)); !written) {
    return Error(E_IO, written.error().message);
  }
  stored_ = true;
  return {};
}

void CertificateStore::remove() {
  if (auto removed = node_file::remove(path_); !removed) {
    log().warning << removed.error().message;
    return;
  }
  stored_ = false;
}

} // namespace pp
