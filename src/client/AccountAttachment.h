#ifndef PP_LEDGER_ACCOUNT_ATTACHMENT_H
#define PP_LEDGER_ACCOUNT_ATTACHMENT_H

#include "common/ResultOrError.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace pp {

/**
 * Versioned multi-profile account attachment stored in UserAccount.meta and on
 * live AccountBuffer accounts. Hard-cut: non-empty meta must decode as this
 * envelope (empty string == empty profiles). See docs/name-directory.md.
 */
struct AccountAttachment {
  constexpr static uint32_t VERSION = 1;

  constexpr static uint8_t MODE_USER = 0;
  constexpr static uint8_t MODE_ATTESTED = 1;

  struct Attestation {
    uint64_t publisher_wallet_id{0};
    std::string domain;
    int64_t expires{0};
    std::string sig;

    template <typename Archive> void serialize(Archive &ar) {
      ar &publisher_wallet_id &domain &expires &sig;
    }
  };

  struct ProfileSlot {
    uint8_t mode{MODE_USER};
    std::string data;
    uint64_t seq{0};
    bool has_attestation{false};
    Attestation attestation{};

    template <typename Archive> void serialize(Archive &ar) {
      ar &mode &data &seq &has_attestation &attestation;
    }
  };

  std::map<uint64_t, ProfileSlot> profiles;

  template <typename Archive> void serialize(Archive &ar) { ar &profiles; }

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  static constexpr int32_t E_FORMAT = 1;
  static constexpr int32_t E_PROFILE = 2;

  static AccountAttachment empty();
  static std::string emptySerialized();

  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);

  /**
   * Structural validation. Does not verify attestation crypto or DomainIndex.
   * publisherExists(id) must be true for every profile map key.
   */
  Roe<void> validateStructural(
      const std::function<bool(uint64_t publisherId)> &publisherExists) const;

  /** Empty input => empty attachment. Non-empty must parse + validate. */
  static Roe<AccountAttachment> parseAndValidate(
      const std::string &raw,
      const std::function<bool(uint64_t publisherId)> &publisherExists);
};

} // namespace pp

#endif
