#pragma once
// Key backups (docs/contracts/KEY_BACKUP.md): one ML-DSA-65 key in a file of its own, encrypted under a
// passphrase — what wallets (pp-ledger-apps' wallet, pp-browser's wallet plugins) export to and import
// from, so a person can always take their key from one to another. No network code.

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pp::operation {

inline constexpr const char *kKeyBackupFormat = "pp-ledger-key-backup";
inline constexpr uint32_t kKeyBackupVersion = 1;
/** The shortest passphrase a backup is written under. */
inline constexpr size_t kKeyBackupMinPassphrase = 10;

/** Argon2id's cost: operations and memory (bytes). */
struct KeyBackupLimits {
  uint64_t opsLimit{0};
  uint64_t memLimit{0};
  /** libsodium's sensitive limits (1 GiB): the default. */
  static KeyBackupLimits sensitive();
  /** libsodium's moderate limits (256 MiB): for writers short of memory (a browser). */
  static KeyBackupLimits moderate();
  /** The least libsodium allows: tests only. */
  static KeyBackupLimits minimum();
};

struct BackedUpKey {
  std::string label;
  /** Raw ML-DSA-65 keys. */
  std::string publicKey;
  std::string privateKey;
};

/** `key` encrypted under `passphrase` (at least kKeyBackupMinPassphrase characters): the file's JSON. */
pp::Roe<std::string> keyBackupToJson(const BackedUpKey &key, std::string_view passphrase,
                                     const KeyBackupLimits &limits = KeyBackupLimits::sensitive());
/**
 * The key a backup holds: decrypted with `passphrase`, the pair checked. Refused for another format or
 * version, costs above the sensitive limits, a wrong passphrase or a damaged file.
 */
pp::Roe<BackedUpKey> keyBackupFromJson(const std::string &json, std::string_view passphrase);
/** The public key a backup names (raw), read without the passphrase. */
pp::Roe<std::string> keyBackupPublicKey(const std::string &json);

} // namespace pp::operation
