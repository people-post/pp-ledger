#ifndef PP_LEDGER_CRYPTO_H
#define PP_LEDGER_CRYPTO_H

#include <cstdint>
#include <string>

namespace pp {

class Crypto {
public:
  /** ML-DSA-65 (FIPS 204). Sole supported account key type (PQ-only). */
  static constexpr uint8_t TK_ML_DSA_65 = 1;

  bool isSupported(uint8_t keyType) const;
  std::string name(uint8_t keyType) const;
  bool verify(uint8_t keyType, const std::string &publicKey,
              const std::string &message, const std::string &signature) const;
};

} // namespace pp

#endif // PP_LEDGER_CRYPTO_H
