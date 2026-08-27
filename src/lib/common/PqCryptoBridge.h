#ifndef PP_LEDGER_PQ_CRYPTO_BRIDGE_H
#define PP_LEDGER_PQ_CRYPTO_BRIDGE_H

#include <cstddef>
#include <string>

namespace pp {
namespace pq_bridge {

/** ML-DSA-65 sizes (FIPS 204) — matches pp-cpp-crypto. */
inline constexpr size_t kPublicKeyBytes = 1952;
inline constexpr size_t kSecretKeyBytes = 4032;
inline constexpr size_t kSignatureBytes = 3309;

struct KeyPair {
  std::string publicKey;
  std::string privateKey;
};

/**
 * Thin bridge to pp::MlDsa that avoids pulling pp-cpp-common Error/Roe into
 * ledger TUs (ledger still has its own ResultOrError / Error types).
 * On failure, returns a non-empty error string; on success, out params are set
 * and the returned string is empty.
 */
std::string generateKeyPair(KeyPair &out);
std::string sign(const std::string &privateKey, const std::string &message,
                 std::string &signatureOut);
bool verify(const std::string &publicKey, const std::string &message,
            const std::string &signature);
bool isValidPublicKeyRaw(const std::string &rawPublicKey);

} // namespace pq_bridge
} // namespace pp

#endif // PP_LEDGER_PQ_CRYPTO_BRIDGE_H
