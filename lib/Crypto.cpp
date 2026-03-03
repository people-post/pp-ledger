#include "Crypto.h"
#include "Utilities.h"

namespace pp {

bool Crypto::isSupported(uint8_t keyType) const {
  switch (keyType) {
  case TK_ED25519:
    return true;
  default:
    return false;
  }
}

std::string Crypto::name(uint8_t keyType) const {
  switch (keyType) {
  case TK_ED25519:
    return "Ed25519";
  default:
    return "unknown";
  }
}

bool Crypto::verify(uint8_t keyType, const std::string &publicKey,
                    const std::string &message,
                    const std::string &signature) const {
  switch (keyType) {
  case TK_ED25519:
    return utl::ed25519Verify(publicKey, message, signature);
  default:
    return false;
  }
}

} // namespace pp
