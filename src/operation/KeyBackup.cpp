#include "ledger/operation/KeyBackup.h"

#include "common/io/Json.h"
#include "ledger/lib/common/Utilities.h"

#include <sodium.h>

#include <memory>

namespace pp::operation {

using pp::common::Object;

namespace {

constexpr const char *kKdf = "argon2id13";
constexpr const char *kCipher = "xchacha20poly1305-ietf";
constexpr const char *kKeyType = "ml-dsa-65";

/** What the ciphertext is bound to: the format and the public key named in the clear. */
std::string aadFor(const std::string &publicKey) { return std::string("pp-ledger/key-backup/v1|") + utl::hexEncode(publicKey); }

/** Overwritten when it goes out of scope. */
struct Wiped {
  std::string value;
  ~Wiped() {
    if (!value.empty()) {
      sodium_memzero(value.data(), value.size());
    }
  }
};

pp::Roe<std::string> derive(std::string_view passphrase, const std::string &salt, const KeyBackupLimits &limits) {
  std::string key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES, '\0');
  if (crypto_pwhash(reinterpret_cast<unsigned char *>(key.data()), key.size(), passphrase.data(), passphrase.size(),
                    reinterpret_cast<const unsigned char *>(salt.data()), limits.opsLimit,
                    static_cast<size_t>(limits.memLimit), crypto_pwhash_ALG_ARGON2ID13) != 0) {
    return pp::Error("Deriving the key from the passphrase failed (out of memory?)");
  }
  return key;
}

pp::Roe<std::string> hexField(const Object &o, const char *name, size_t size) {
  std::string hex = o.getString(name).value_or("");
  if (hex.rfind("0x", 0) == 0) {
    hex = hex.substr(2);
  }
  std::string raw = utl::hexDecode(hex);
  if (raw.size() != size || (size == 0 && raw.empty())) {
    return pp::Error(std::string("The key backup's ") + name + " is malformed");
  }
  return raw;
}

pp::Roe<Object> readHeader(const std::string &json) {
  Object root;
  if (!pp::common::io::objectFromJsonString(root, json)) {
    return pp::Error("Not a key backup: not JSON");
  }
  if (root.getString("format").value_or("") != kKeyBackupFormat) {
    return pp::Error("Not a key backup");
  }
  if (root.getNonNegInt("version").value_or(0) != kKeyBackupVersion) {
    return pp::Error("A key backup of another version");
  }
  if (root.getString("keyType").value_or("") != kKeyType) {
    return pp::Error("A key backup of another kind of key");
  }
  return root;
}

} // namespace

KeyBackupLimits KeyBackupLimits::sensitive() {
  return {crypto_pwhash_OPSLIMIT_SENSITIVE, crypto_pwhash_MEMLIMIT_SENSITIVE};
}

KeyBackupLimits KeyBackupLimits::moderate() { return {crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE}; }

KeyBackupLimits KeyBackupLimits::minimum() { return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN}; }

pp::Roe<std::string> keyBackupToJson(const BackedUpKey &key, std::string_view passphrase,
                                     const KeyBackupLimits &limits) {
  if (sodium_init() < 0) {
    return pp::Error("No randomness");
  }
  if (passphrase.size() < kKeyBackupMinPassphrase) {
    return pp::Error("The passphrase needs at least " + std::to_string(kKeyBackupMinPassphrase) + " characters");
  }
  if (key.publicKey.size() != utl::kMlDsaPublicKeyBytes || key.privateKey.size() != utl::kMlDsaPrivateKeyBytes) {
    return pp::Error("Not an ML-DSA-65 key pair");
  }
  std::string salt(crypto_pwhash_SALTBYTES, '\0');
  randombytes_buf(salt.data(), salt.size());
  std::string nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, '\0');
  randombytes_buf(nonce.data(), nonce.size());
  auto derived = derive(passphrase, salt, limits);
  if (!derived) {
    return derived.error();
  }
  Wiped secret{std::move(derived.value())};
  Object inner;
  inner.set("label", key.label);
  inner.set("privateKey", utl::hexEncode(key.privateKey));
  Wiped plain{pp::common::io::objectToJsonString(inner)};
  const std::string aad = aadFor(key.publicKey);
  std::string sealed(plain.value.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
  unsigned long long length = 0;
  crypto_aead_xchacha20poly1305_ietf_encrypt(
      reinterpret_cast<unsigned char *>(sealed.data()), &length, reinterpret_cast<const unsigned char *>(plain.value.data()),
      plain.value.size(), reinterpret_cast<const unsigned char *>(aad.data()), aad.size(), nullptr,
      reinterpret_cast<const unsigned char *>(nonce.data()), reinterpret_cast<const unsigned char *>(secret.value.data()));
  sealed.resize(static_cast<size_t>(length));

  Object kdf;
  kdf.set("algorithm", kKdf);
  kdf.setJsonUInt("opsLimit", limits.opsLimit);
  kdf.setJsonUInt("memLimit", limits.memLimit);
  kdf.set("salt", utl::hexEncode(salt));
  Object cipher;
  cipher.set("algorithm", kCipher);
  cipher.set("nonce", utl::hexEncode(nonce));
  Object root;
  root.set("format", kKeyBackupFormat);
  root.setJsonUInt("version", kKeyBackupVersion);
  root.set("keyType", kKeyType);
  root.set("publicKey", utl::hexEncode(key.publicKey));
  root.set("kdf", kdf);
  root.set("cipher", cipher);
  root.set("ciphertext", utl::hexEncode(sealed));
  return pp::common::io::objectToJsonString(root, 2) + "\n";
}

pp::Roe<std::string> keyBackupPublicKey(const std::string &json) {
  auto root = readHeader(json);
  if (!root) {
    return root.error();
  }
  return hexField(root.value(), "publicKey", utl::kMlDsaPublicKeyBytes);
}

pp::Roe<BackedUpKey> keyBackupFromJson(const std::string &json, std::string_view passphrase) {
  if (sodium_init() < 0) {
    return pp::Error("No randomness");
  }
  auto root = readHeader(json);
  if (!root) {
    return root.error();
  }
  auto publicKey = hexField(root.value(), "publicKey", utl::kMlDsaPublicKeyBytes);
  const Object *kdf = root.value().getObject("kdf");
  const Object *cipher = root.value().getObject("cipher");
  if (!publicKey || !kdf || !cipher || kdf->getString("algorithm").value_or("") != kKdf ||
      cipher->getString("algorithm").value_or("") != kCipher) {
    return pp::Error("The key backup names an encryption this reader does not know");
  }
  KeyBackupLimits limits{kdf->getNonNegInt("opsLimit").value_or(0), kdf->getNonNegInt("memLimit").value_or(0)};
  // A file asking for more than the sensitive limits would only exhaust the reader.
  if (limits.opsLimit < crypto_pwhash_OPSLIMIT_MIN || limits.opsLimit > crypto_pwhash_OPSLIMIT_SENSITIVE ||
      limits.memLimit < crypto_pwhash_MEMLIMIT_MIN || limits.memLimit > crypto_pwhash_MEMLIMIT_SENSITIVE) {
    return pp::Error("The key backup's passphrase costs are out of range");
  }
  auto salt = hexField(*kdf, "salt", crypto_pwhash_SALTBYTES);
  auto nonce = hexField(*cipher, "nonce", crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  std::string hex = root.value().getString("ciphertext").value_or("");
  const std::string sealed = utl::hexDecode(hex);
  if (!salt || !nonce || sealed.size() <= crypto_aead_xchacha20poly1305_ietf_ABYTES) {
    return pp::Error("The key backup is damaged");
  }
  auto derived = derive(passphrase, salt.value(), limits);
  if (!derived) {
    return derived.error();
  }
  Wiped secret{std::move(derived.value())};
  const std::string aad = aadFor(publicKey.value());
  Wiped plain{std::string(sealed.size(), '\0')};
  unsigned long long length = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          reinterpret_cast<unsigned char *>(plain.value.data()), &length, nullptr,
          reinterpret_cast<const unsigned char *>(sealed.data()), sealed.size(),
          reinterpret_cast<const unsigned char *>(aad.data()), aad.size(),
          reinterpret_cast<const unsigned char *>(nonce.value().data()),
          reinterpret_cast<const unsigned char *>(secret.value.data())) != 0) {
    return pp::Error("Wrong passphrase, or the key backup is damaged");
  }
  plain.value.resize(static_cast<size_t>(length));
  Object inner;
  if (!pp::common::io::objectFromJsonString(inner, plain.value)) {
    return pp::Error("The key backup is damaged");
  }
  BackedUpKey key;
  key.label = inner.getString("label").value_or("");
  key.publicKey = publicKey.value();
  Wiped privateHex{inner.getString("privateKey").value_or("")};
  key.privateKey = utl::hexDecode(privateHex.value);
  if (key.privateKey.size() != utl::kMlDsaPrivateKeyBytes) {
    return pp::Error("The key backup is damaged");
  }
  // The pair: the private key signs what the public key verifies.
  static const std::string probe = "pp-ledger/key-backup/pair-probe/v1";
  auto signature = utl::mlDsaSign(key.privateKey, probe);
  if (!signature || !utl::mlDsaVerify(key.publicKey, probe, signature.value())) {
    sodium_memzero(key.privateKey.data(), key.privateKey.size());
    return pp::Error("The key backup's keys are not a pair");
  }
  return key;
}

} // namespace pp::operation
