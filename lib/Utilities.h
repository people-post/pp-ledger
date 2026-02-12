#ifndef PP_LEDGER_UTILITIES_H
#define PP_LEDGER_UTILITIES_H

#include <string>
#include <cstdint>
#include "ResultOrError.hpp"
#include <nlohmann/json.hpp>

namespace pp {

// Error type for utility functions
struct Error : public RoeErrorBase {
  Error() : RoeErrorBase() {}
  Error(int32_t c, const std::string &msg) : RoeErrorBase(c, msg) {}
  Error(int32_t c, std::string &&msg) : RoeErrorBase(c, std::move(msg)) {}
  explicit Error(const std::string &msg) : RoeErrorBase(msg) {}
  explicit Error(std::string &&msg) : RoeErrorBase(std::move(msg)) {}
};

template <typename T> using Roe = ResultOrError<T, Error>;

namespace utl {

/**
 * Get the current time in seconds since the epoch
 * @return Current time in seconds
 */
int64_t getCurrentTime();

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
 * Join a vector of strings with a delimiter
 * @param strings Vector of strings to join
 * @param delimiter Delimiter to insert between strings
 * @return Joined string
 */
std::string join(const std::vector<std::string> &strings, const std::string &delimiter);

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

// --- Ed25519 (raw binary: 32-byte public key, 32-byte private key, 64-byte signature)

/** Ed25519 key pair: publicKey (32 bytes), privateKey (32 bytes) */
struct Ed25519KeyPair {
  std::string publicKey;
  std::string privateKey;
};

/**
 * Generate a new Ed25519 key pair
 * @return Roe<Ed25519KeyPair>: publicKey and privateKey as 32-byte binary strings, or error
 */
pp::Roe<Ed25519KeyPair> ed25519Generate();

/**
 * Sign a message with an Ed25519 private key
 * @param privateKey 32-byte raw private key (from ed25519Generate or equivalent)
 * @param message Message to sign (arbitrary bytes)
 * @return Roe<std::string>: 64-byte signature, or error
 */
pp::Roe<std::string> ed25519Sign(const std::string &privateKey, const std::string &message);

/**
 * Verify an Ed25519 signature
 * @param publicKey 32-byte raw public key
 * @param message Message that was signed
 * @param signature 64-byte signature
 * @return true if signature is valid, false if invalid or bad key/signature format
 */
bool ed25519Verify(const std::string &publicKey, const std::string &message,
                   const std::string &signature);

/**
 * Check if a string is a valid ED25519 public key.
 * Accepts either 32-byte raw binary, or 64 hex characters (optionally with "0x" prefix).
 * @param str String to validate (raw 32 bytes, or hex-encoded key)
 * @return true if the string represents a valid ED25519 public key, false otherwise
 */
bool isValidEd25519PublicKey(const std::string &str);

/** Alias for isValidEd25519PublicKey (e.g. for config validation). */
inline bool isValidPublicKey(const std::string &str) {
  return isValidEd25519PublicKey(str);
}

/** 
 * Read a key string or file containing hex-encoded private key
 * @param key String or file containing hex-encoded private key
 * @return Hex-encoded private key
 */
std::string readKey(const std::string& key);

} // namespace utl
} // namespace pp

#endif // PP_LEDGER_UTILITIES_H