#include "CertificateStore.h"
#include "lib/common/BinaryPack.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace pp {

CertificateStore::CertificateStore(Chain &chain, std::string path) : chain_(chain), path_(std::move(path)) {
  redirectLogger("CertificateStore");
}

CertificateStore::Roe<void> CertificateStore::load() {
  pending_.reset();
  if (!std::filesystem::exists(path_)) {
    return {};
  }
  std::string bytes;
  {
    // Closed before anything below may remove the file (Windows cannot remove an open file).
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
      return Error(E_IO, "Failed to read " + path_);
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
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

CertificateStore::Roe<void> CertificateStore::persist(const Ledger::Record &record) const {
  // Write aside, then rename over: a crash leaves the old certificate or the new one.
  const std::string temp = path_ + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << utl::binaryPack(record);
    out.flush();
    if (!out) {
      return Error(E_IO, "Failed to write " + temp);
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path_, ec);
  if (ec) {
    return Error(E_IO, "Failed to replace " + path_ + ": " + ec.message());
  }
  return {};
}

void CertificateStore::remove() const {
  std::error_code ec;
  std::filesystem::remove(path_, ec);
  if (ec) {
    log().warning << "Failed to remove " << path_ << ": " << ec.message();
  }
}

} // namespace pp
