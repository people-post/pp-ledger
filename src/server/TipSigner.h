#ifndef PP_LEDGER_TIP_SIGNER_H
#define PP_LEDGER_TIP_SIGNER_H

#include "../chain/Types.h"
#include "../client/Client.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Utilities.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pp {

/**
 * The beacon's signed tip (beacon-authority ba3, B009): signs each block the beacon commits and
 * keeps only the latest, written to disk before it is served. It never signs a different block at
 * or below the last signed height, so a crash between signing and storing cannot make an honest
 * beacon sign two tips at one height. Server thread only.
 */
class TipSigner : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  static constexpr const int32_t E_IO = 1;
  /** The certificate in force names another key: this beacon may not sign. */
  static constexpr const int32_t E_NOT_CERTIFIED = 2;
  /** Signing would put a second tip at or below the last signed height. */
  static constexpr const int32_t E_CONFLICT = 3;

  TipSigner(std::string path, utl::MlDsaKeyPair key);

  /** Read the persisted tip (none yet on a fresh beacon). */
  Roe<void> load();

  const std::optional<Client::SignedTip> &latest() const { return latest_; }
  const std::string &publicKey() const { return key_.publicKey; }

  /**
   * Sign block `height` (hash `blockHash`) of `cert`'s chain as `cert`'s generation, then persist
   * it. Signing the latest again is a no-op.
   */
  Roe<void> sign(const BeaconCertificate &cert, uint64_t height, const std::string &blockHash);

private:
  Roe<void> persist(const Client::SignedTip &tip) const;

  std::string path_;
  utl::MlDsaKeyPair key_;
  std::optional<Client::SignedTip> latest_;
};

} // namespace pp

#endif // PP_LEDGER_TIP_SIGNER_H
