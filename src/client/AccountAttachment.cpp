#include "AccountAttachment.h"
#include "AccountIds.h"

#include "common/Serialize.hpp"

#include <sstream>

namespace pp {

AccountAttachment AccountAttachment::empty() { return AccountAttachment{}; }

std::string AccountAttachment::emptySerialized() {
  return empty().ltsToString();
}

std::string AccountAttachment::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar &VERSION &*this;
  return oss.str();
}

bool AccountAttachment::ltsFromString(const std::string &str) {
  if (str.empty()) {
    profiles.clear();
    return true;
  }
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar &version;
  if (version != VERSION) {
    return false;
  }
  ar &*this;
  return !ar.failed();
}

AccountAttachment::Roe<void> AccountAttachment::validateStructural(
    const std::function<bool(uint64_t publisherId)> &publisherExists) const {
  if (!publisherExists) {
    return Error(E_FORMAT, "publisherExists predicate required");
  }
  for (const auto &[publisherId, slot] : profiles) {
    if (!publisherExists(publisherId)) {
      return Error(E_PROFILE, "profile_id is not a known wallet id: " +
                                  std::to_string(publisherId));
    }
    if (slot.mode != MODE_USER && slot.mode != MODE_ATTESTED) {
      return Error(E_FORMAT, "invalid profile slot mode");
    }
    if (slot.mode == MODE_ATTESTED) {
      if (!slot.has_attestation) {
        return Error(E_FORMAT, "attested slot missing attestation");
      }
      if (slot.attestation.publisher_wallet_id != publisherId) {
        return Error(E_FORMAT, "attestation publisher_wallet_id must equal "
                               "profile map key");
      }
      // Reserved band only (DomainIndex check deferred).
      if (publisherId >= AccountIds::ID_FIRST_USER) {
        return Error(E_PROFILE,
                     "attested profile_id must be a reserved account");
      }
      if (slot.attestation.domain.empty()) {
        return Error(E_FORMAT, "attested slot requires non-empty domain");
      }
      if (slot.attestation.sig.empty()) {
        return Error(E_FORMAT, "attested slot requires non-empty signature");
      }
    }
  }
  return {};
}

AccountAttachment::Roe<AccountAttachment> AccountAttachment::parseAndValidate(
    const std::string &raw,
    const std::function<bool(uint64_t publisherId)> &publisherExists) {
  AccountAttachment att;
  if (!att.ltsFromString(raw)) {
    return Error(E_FORMAT, "invalid AccountAttachment envelope");
  }
  if (auto r = att.validateStructural(publisherExists); !r) {
    return r.error();
  }
  return att;
}

} // namespace pp
