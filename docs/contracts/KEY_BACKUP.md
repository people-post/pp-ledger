# Key backup

One account key in a file of its own, encrypted under a passphrase: what wallets export to and import
from, so a person can always take their key from one wallet to another — from a pp-browser wallet
plugin to pp-ledger-apps' wallet, their last resort, and back. **A wallet that holds a person's keys
must be able to export them in this format.** Code: `include/ledger/operation/KeyBackup.h` (network-free);
`pp-client key-export` / `key-import`.

## Format

UTF-8 JSON, one object. Byte strings are lowercase hex.

| Field | Holds |
|-------|-------|
| `format` | `pp-ledger-key-backup` |
| `version` | `1`; a reader refuses any other |
| `keyType` | `ml-dsa-65` |
| `publicKey` | The raw public key (1952 bytes), in the clear: which key this is, before the passphrase |
| `kdf` | `algorithm` `argon2id13`; `opsLimit`, `memLimit` (bytes); `salt` (16 bytes) |
| `cipher` | `algorithm` `xchacha20poly1305-ietf`; `nonce` (24 bytes) |
| `ciphertext` | The sealed JSON `{"label": …, "privateKey": hex}`, its tag included |

- **Key**: Argon2id (`crypto_pwhash`, `ARGON2ID13`) of the passphrase under `salt`, `opsLimit`,
  `memLimit`, 32 bytes.
- **Seal**: XChaCha20-Poly1305 IETF (`crypto_aead_xchacha20poly1305_ietf_encrypt`) with additional
  data `pp-ledger/key-backup/v1|` followed by `publicKey` in hex: the ciphertext belongs to that public
  key only. The label is sealed too: a person's names for their keys are theirs.
- All of it is libsodium's, available to native code and to browsers (libsodium.js).

## Rules

- **Writing**: a passphrase of 10 characters at least; costs of libsodium's *sensitive* limits
  (`opsLimit` 4, `memLimit` 1 GiB) by default, *moderate* (3, 256 MiB) where memory is short.
- **Reading**: refused for another format, version, key type or algorithm; for costs below libsodium's
  minimum or above the sensitive limits (a file must not exhaust its reader); for a wrong passphrase or
  a damaged file; and when the private key does not sign what the public key verifies.
- A backup is as strong as its passphrase: anyone holding the file can try passphrases offline.
