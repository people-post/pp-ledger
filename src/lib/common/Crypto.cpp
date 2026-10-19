#include "ledger/lib/common/Crypto.h"
#include "ledger/lib/common/Utilities.h"

namespace pp {

bool Crypto::isSupported(uint8_t keyType) const {
  switch (keyType) {
  case TK_ML_DSA_65:
    return true;
  default:
    return false;
  }
}

std::string Crypto::name(uint8_t keyType) const {
  switch (keyType) {
  case TK_ML_DSA_65:
    return "ML-DSA-65";
  default:
    return "unknown";
  }
}

bool Crypto::verify(uint8_t keyType, const std::string &publicKey,
                    const std::string &message,
                    const std::string &signature) const {
  switch (keyType) {
  case TK_ML_DSA_65:
    return utl::mlDsaVerify(publicKey, message, signature);
  default:
    return false;
  }
}

} // namespace pp
