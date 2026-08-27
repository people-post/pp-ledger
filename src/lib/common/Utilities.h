#ifndef PP_LEDGER_UTILITIES_H
#define PP_LEDGER_UTILITIES_H

#include <cstddef>
#include <string>
#include <cstdint>
#include <sstream>
#include <vector>
#include "Meta.h"
#include "common/Error.h"
#include <json.hpp>

namespace pp {

namespace utl {

/**
 * Get the current time in seconds since the epoch
 * @return Current time in seconds
 */
int64_t getCurrentTime();

/**
 * Format a Unix timestamp as a human-readable local time string
 * @param unixSeconds Seconds since 1970-01-01 00:00:00 UTC
 * @return Formatted string (e.g. "2025-02-13 12:34:56 PST") or raw number as string on failure
 */
std::string formatTimestampLocal(int64_t unixSeconds);

/**
 * Parse an integer from a string
 * @param str String to parse
 * @param value Output parameter for the parsed value
 * @return true if parsing succeeded, false otherwise
 */
bool parseInt(const std::string &str, int &value);

/**
 * Parse a 64-bit signed integer from a string
 * @param str String to parse
 * @param value Output parameter for the parsed value
 * @return true if parsing succeeded, false otherwise
 */
bool parseInt64(const std::string &str, int64_t &value);

/**
 * Parse a 64-bit unsigned integer from a string
 * @param str String to parse
 * @param value Output parameter for the parsed value
 * @return true if parsing succeeded, false otherwise
 */
bool parseUInt64(const std::string &str, uint64_t &value);

/**
 * Parse a port number from a string (validates range 0-65535)
 * @param str String to parse
 * @param port Output parameter for the parsed port
 * @return true if parsing succeeded and port is in valid range, false otherwise
 */
bool parsePort(const std::string &str, uint16_t &port);

/**
 * Parse a host:port string into separate host and port components
 * @param hostPort String in format "host:port"
 * @param host Output parameter for the host part
 * @param port Output parameter for the port part
 * @return true if parsing succeeded, false otherwise
 */
bool parseHostPort(const std::string &hostPort, std::string &host, uint16_t &port);

/**
 * Join a vector of streamable values with a delimiter
 * @param values Vector of values to join
 * @param delimiter Delimiter to insert between strings
 * @return Joined string
 */
template <typename T>
std::string join(const std::vector<T> &values, const std::string &delimiter) {
  if (values.empty()) {
    return "";
  }

  std::ostringstream result;
  result << values[0];
  for (size_t i = 1; i < values.size(); ++i) {
    result << delimiter << values[i];
  }
  return result.str();
}

/**
 * Load and parse a JSON configuration file
 * @param configPath Path to the JSON configuration file
 * @param config Output parameter for the parsed JSON object
 * @return Roe<void> indicating success or error
 */
pp::Roe<nlohmann::json> loadJsonFile(const std::string &configPath);

/**
 * Parse and validate a JSON request string
 * @param request The JSON request string to parse
 * @param reqJson Output parameter for the parsed JSON object
 * @return Roe<void> indicating success or error
 */
pp::Roe<nlohmann::json> parseJsonRequest(const std::string &request);

/**
 * Compute SHA-256 hash using Libsodium
 * @param input Input string to hash
 * @return Hexadecimal string representation of the SHA-256 hash
 * @throws std::runtime_error if hash computation fails
 */
std::string sha256(const std::string &input);

/**
 * Encode binary data as hex string (e.g. for JSON-safe transport)
 * @param data Raw bytes
 * @return Lowercase hex string (two chars per byte)
 */
std::string hexEncode(const std::string &data);

/**
 * Decode hex string back to binary
 * @param hex Hex string (even length, 0-9a-fA-F)
 * @return Decoded bytes, or empty string if input is invalid
 */
std::string hexDecode(const std::string &hex);

/**
 * Return a string safe for JSON (UTF-8). If input contains non-UTF-8 bytes,
 * returns "0x" + hexEncode(input) so the receiver can hexDecode.
 * @param s Arbitrary string (may be binary)
 * @return UTF-8-safe string
 */
std::string toJsonSafeString(const std::string &s);

/**
 * Reverse of toJsonSafeString: if string starts with "0x", hex-decode the rest.
 * @param s String from JSON (either plain or "0x" + hex)
 * @return Decoded binary or original string
 */
std::string fromJsonSafeString(const std::string &s);

/**
 * Write a string to a non-existent file
 * Creates parent directories if needed. Fails if the file already exists.
 * @param filePath Path to the file to write
 * @param content String content to write to the file
 * @return Roe<void> indicating success or error
 */
pp::Roe<void> writeToNewFile(const std::string &filePath, const std::string &content);

// --- ML-DSA-65 (raw binary: 1952-byte public key, 4032-byte private key, 3309-byte signature)

/** ML-DSA-65 key pair sizes (FIPS 204 via pp-cpp-crypto). */
inline constexpr size_t kMlDsaPublicKeyBytes = 1952;
inline constexpr size_t kMlDsaPrivateKeyBytes = 4032;
inline constexpr size_t kMlDsaSignatureBytes = 3309;

/** ML-DSA-65 key pair: publicKey / privateKey as raw binary strings. */
struct MlDsaKeyPair {
  std::string publicKey;
  std::string privateKey;

  /** JSON/meta object with hex-encoded publicKey and privateKey (init key export). */
  pp::common::Meta ltsToMeta() const;
};

/**
 * Generate a new ML-DSA-65 key pair
 * @return Roe<MlDsaKeyPair>: raw binary public/private keys, or error
 */
pp::Roe<MlDsaKeyPair> mlDsaGenerate();

/**
 * Sign a message with an ML-DSA-65 private key
 * @param privateKey Raw private key (kMlDsaPrivateKeyBytes)
 * @param message Message to sign (arbitrary bytes)
 * @return Roe<std::string>: signature (kMlDsaSignatureBytes), or error
 */
pp::Roe<std::string> mlDsaSign(const std::string &privateKey, const std::string &message);

/**
 * Verify an ML-DSA-65 signature
 * @param publicKey Raw public key (kMlDsaPublicKeyBytes)
 * @param message Message that was signed
 * @param signature Raw signature (kMlDsaSignatureBytes)
 * @return true if signature is valid, false if invalid or bad key/signature format
 */
bool mlDsaVerify(const std::string &publicKey, const std::string &message,
                 const std::string &signature);

/**
 * Check if a string is a plausible ML-DSA-65 public key.
 * Accepts raw kMlDsaPublicKeyBytes, or hex (optionally with "0x" prefix).
 */
bool isValidMlDsaPublicKey(const std::string &str);

/** Alias for isValidMlDsaPublicKey (e.g. for config validation). */
inline bool isValidPublicKey(const std::string &str) {
  return isValidMlDsaPublicKey(str);
}

/**
 * Read a key string or file containing hex-encoded private key
 * @param key String or file containing hex-encoded private key
 * @return Hex-encoded private key (trimmed)
 */
std::string readKey(const std::string& key);

/**
 * Read private key from file path (relative to baseDir if relative) or inline hex string.
 * Supports hex-encoded (2 * kMlDsaPrivateKeyBytes hex chars, optionally 0x prefix)
 * or raw kMlDsaPrivateKeyBytes.
 * @param keyOrPath File path (relative to baseDir) or inline hex private key
 * @param baseDir Base directory for resolving relative paths (e.g. config file directory)
 * @return Roe<std::string>: raw private key, or error
 */
pp::Roe<std::string> readPrivateKey(const std::string& keyOrPath,
                                    const std::string& baseDir);

} // namespace utl
} // namespace pp

#endif // PP_LEDGER_UTILITIES_H