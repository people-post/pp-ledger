#include "PqCryptoBridge.h"

#include "crypto/MlDsa.h"
#include "crypto/Types.h"

#include <cstdint>
#include <utility>

namespace pp {
namespace pq_bridge {
namespace {

ByteVector toBytes(const std::string &s) {
  return ByteVector(reinterpret_cast<const uint8_t *>(s.data()),
                    reinterpret_cast<const uint8_t *>(s.data()) + s.size());
}

std::string fromBytes(const ByteVector &v) {
  return std::string(reinterpret_cast<const char *>(v.data()), v.size());
}

} // namespace

std::string generateKeyPair(KeyPair &out) {
  auto result = MlDsa::GenerateKeyPair();
  if (result.isError()) {
    return result.error().message.empty() ? "ML-DSA-65 keygen failed"
                                          : result.error().message;
  }
  out.publicKey = fromBytes(result->public_key);
  out.privateKey = fromBytes(result->secret_key);
  return {};
}

std::string sign(const std::string &privateKey, const std::string &message,
                 std::string &signatureOut) {
  if (privateKey.size() != kSecretKeyBytes) {
    return "mlDsaSign: private key must be " + std::to_string(kSecretKeyBytes) +
           " bytes";
  }
  auto result = MlDsa::Sign(toBytes(privateKey), toBytes(message));
  if (result.isError()) {
    return result.error().message.empty() ? "ML-DSA-65 sign failed"
                                          : result.error().message;
  }
  signatureOut = fromBytes(*result);
  return {};
}

bool verify(const std::string &publicKey, const std::string &message,
            const std::string &signature) {
  if (publicKey.size() != kPublicKeyBytes ||
      signature.size() != kSignatureBytes) {
    return false;
  }
  auto result =
      MlDsa::Verify(toBytes(publicKey), toBytes(message), toBytes(signature));
  if (result.isError()) {
    return false;
  }
  return *result;
}

bool isValidPublicKeyRaw(const std::string &rawPublicKey) {
  if (rawPublicKey.size() != kPublicKeyBytes) {
    return false;
  }
  // Round-trip verify against an empty message with a zero signature is not a
  // structural check. Accept exact-size keys; ML-DSA verify rejects bad keys
  // at signature check time. Reject all-zero keys as obviously invalid.
  for (unsigned char c : rawPublicKey) {
    if (c != 0) {
      return true;
    }
  }
  return false;
}

} // namespace pq_bridge
} // namespace pp
