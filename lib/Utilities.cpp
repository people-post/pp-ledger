#include "Utilities.h"
#include <charconv>
#include <ctime>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <sodium.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace pp {
namespace utl {

// Initialize libsodium (safe to call multiple times)
namespace {
  struct SodiumInitializer {
    SodiumInitializer() {
      if (sodium_init() < 0) {
        throw std::runtime_error("Failed to initialize libsodium");
      }
    }
  };
  static SodiumInitializer sodium_initializer;
}

int64_t getCurrentTime() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string formatTimestampLocal(int64_t unixSeconds) {
  time_t t = static_cast<time_t>(unixSeconds);
  std::tm* local = std::localtime(&t);
  if (!local) return std::to_string(unixSeconds);
  char buf[64];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", local) == 0)
    return std::to_string(unixSeconds);
  return std::string(buf);
}

bool parseInt(const std::string &str, int &value) {
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc{} && ptr == str.data() + str.size();
}

bool parseInt64(const std::string &str, int64_t &value) {
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc{} && ptr == str.data() + str.size();
}

bool parseUInt64(const std::string &str, uint64_t &value) {
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc{} && ptr == str.data() + str.size();
}

bool parsePort(const std::string &str, uint16_t &port) {
  int portInt = 0;
  if (!parseInt(str, portInt)) {
    return false;
  }
  if (portInt < 0 || portInt > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(portInt);
  return true;
}

bool parseHostPort(const std::string &hostPort, std::string &host, uint16_t &port) {
  size_t colonPos = hostPort.find_last_of(':');
  if (colonPos == std::string::npos || colonPos == 0 ||
      colonPos == hostPort.length() - 1) {
    return false;
  }

  host = hostPort.substr(0, colonPos);
  std::string portStr = hostPort.substr(colonPos + 1);
  return parsePort(portStr, port);
}

pp::Roe<nlohmann::json> loadJsonFile(const std::string &configPath) {
  if (!std::filesystem::exists(configPath)) {
    return Error(1, "Configuration file not found: " + configPath);
  }

  std::ifstream configFile(configPath);
  if (!configFile.is_open()) {
    return Error(2, "Failed to open configuration file: " + configPath);
  }

  // Read file content
  std::string content((std::istreambuf_iterator<char>(configFile)),
                      std::istreambuf_iterator<char>());
  configFile.close();

  // Parse JSON
  nlohmann::json config;
  try {
    config = nlohmann::json::parse(content);
  } catch (const nlohmann::json::parse_error &e) {
    return Error(3, "Failed to parse JSON: " + std::string(e.what()));
  }

  return config;
}

pp::Roe<nlohmann::json> parseJsonRequest(const std::string &request) {
  nlohmann::json reqJson;
  try {
    reqJson = nlohmann::json::parse(request);
  } catch (const nlohmann::json::parse_error &e) {
    return Error(1, "Failed to parse request JSON: " + std::string(e.what()));
  }
  
  if (!reqJson.contains("type")) {
    return Error(2, "missing type field");
  }
  
  return reqJson;
}

std::string sha256(const std::string &input) {
  unsigned char hash[crypto_hash_sha256_BYTES];
  
  if (crypto_hash_sha256(hash, 
                         reinterpret_cast<const unsigned char*>(input.c_str()), 
                         input.size()) != 0) {
    throw std::runtime_error("crypto_hash_sha256 failed");
  }

  std::stringstream ss;
  for (unsigned int i = 0; i < crypto_hash_sha256_BYTES; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(hash[i]);
  }
  return ss.str();
}

std::string hexEncode(const std::string &data) {
  std::stringstream ss;
  for (unsigned char c : data) {
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  }
  return ss.str();
}

std::string hexDecode(const std::string &hex) {
  if (hex.size() % 2 != 0) {
    return {};
  }
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = 0, lo = 0;
    char c1 = hex[i], c2 = hex[i + 1];
    if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
    else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
    else return {};
    if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
    else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
    else return {};
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::string toJsonSafeString(const std::string &s) {
  for (unsigned char c : s) {
    if (c >= 128 || (c < 32 && c != ' ')) {
      return "0x" + hexEncode(s);
    }
  }
  return s;
}

std::string fromJsonSafeString(const std::string &s) {
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    return hexDecode(s.substr(2));
  }
  return s;
}

pp::Roe<void> writeToNewFile(const std::string &filePath, const std::string &content) {
  // Check if file already exists
  if (std::filesystem::exists(filePath)) {
    return Error(1, "File already exists: " + filePath);
  }

  // Create parent directories if needed
  std::filesystem::path path(filePath);
  std::filesystem::path parentDir = path.parent_path();
  if (!parentDir.empty() && !std::filesystem::exists(parentDir)) {
    std::error_code ec;
    std::filesystem::create_directories(parentDir, ec);
    if (ec) {
      return Error(2, "Failed to create parent directories for " + filePath + ": " + ec.message());
    }
  }

  // Write content to file
  std::ofstream file(filePath);
  if (!file.is_open()) {
    return Error(3, "Failed to open file for writing: " + filePath);
  }

  file << content;
  file.close();

  if (!file.good()) {
    return Error(4, "Failed to write content to file: " + filePath);
  }

  return {};
}

// --- Ed25519

namespace {

constexpr size_t ED25519_PRIVATE_KEY_SIZE = 32;
constexpr size_t ED25519_PUBLIC_KEY_SIZE = 32;
constexpr size_t ED25519_SIGNATURE_SIZE = 64;

} // namespace

pp::Roe<Ed25519KeyPair> ed25519Generate() {
  Ed25519KeyPair pair;
  pair.publicKey.resize(crypto_sign_PUBLICKEYBYTES);
  pair.privateKey.resize(crypto_sign_SECRETKEYBYTES);
  
  if (crypto_sign_keypair(
        reinterpret_cast<unsigned char*>(pair.publicKey.data()),
        reinterpret_cast<unsigned char*>(pair.privateKey.data())) != 0) {
    return Error(1, "crypto_sign_keypair failed");
  }
  
  // Libsodium's secret key is 64 bytes (32 seed + 32 public), but we only want the 32-byte seed
  pair.privateKey.resize(ED25519_PRIVATE_KEY_SIZE);
  
  return pair;
}

pp::Roe<std::string> ed25519Sign(const std::string &privateKey,
                                 const std::string &message) {
  if (privateKey.size() != ED25519_PRIVATE_KEY_SIZE) {
    return Error(1, "ed25519Sign: private key must be 32 bytes");
  }
  
  // Libsodium expects a 64-byte secret key (32 seed + 32 public key)
  // We need to expand our 32-byte seed to get the full secret key
  std::vector<unsigned char> pk(crypto_sign_PUBLICKEYBYTES);
  std::vector<unsigned char> sk(crypto_sign_SECRETKEYBYTES);
  
  if (crypto_sign_seed_keypair(
        pk.data(), sk.data(),
        reinterpret_cast<const unsigned char*>(privateKey.data())) != 0) {
    return Error(2, "crypto_sign_seed_keypair failed");
  }
  
  std::string signature(crypto_sign_BYTES, '\0');
  unsigned long long sig_len = 0;
  
  if (crypto_sign_detached(
        reinterpret_cast<unsigned char*>(signature.data()),
        &sig_len,
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size(),
        sk.data()) != 0) {
    return Error(3, "crypto_sign_detached failed");
  }
  
  if (sig_len != ED25519_SIGNATURE_SIZE) {
    return Error(4, "unexpected signature size");
  }
  
  return signature;
}

bool ed25519Verify(const std::string &publicKey, const std::string &message,
                  const std::string &signature) {
  if (publicKey.size() != ED25519_PUBLIC_KEY_SIZE ||
      signature.size() != ED25519_SIGNATURE_SIZE) {
    return false;
  }
  
  int result = crypto_sign_verify_detached(
      reinterpret_cast<const unsigned char*>(signature.data()),
      reinterpret_cast<const unsigned char*>(message.data()),
      message.size(),
      reinterpret_cast<const unsigned char*>(publicKey.data()));
  
  return result == 0;
}

bool isValidEd25519PublicKey(const std::string &str) {
  std::string raw;
  if (str.size() == ED25519_PUBLIC_KEY_SIZE) {
    raw = str;
  } else if (str.size() == 64) {
    raw = hexDecode(str);
    if (raw.size() != ED25519_PUBLIC_KEY_SIZE) {
      return false;
    }
  } else if (str.size() == 66 && (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))) {
    raw = hexDecode(str.substr(2));
    if (raw.size() != ED25519_PUBLIC_KEY_SIZE) {
      return false;
    }
  } else {
    return false;
  }
  
  // Validate that the public key is a valid point on the Ed25519 curve
  return crypto_core_ed25519_is_valid_point(
      reinterpret_cast<const unsigned char*>(raw.data())) == 1;
}

static std::string trimWhitespace(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string readKey(const std::string &key) {
  if (key.empty()) {
    return "";
  }
  if (std::filesystem::exists(key)) {
    std::ifstream file(key);
    if (!file.is_open()) {
      return "";
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();
    return trimWhitespace(content);
  }
  return trimWhitespace(key);
}

namespace {

bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool isHexString(const std::string &s, size_t expectedLen) {
  if (s.size() != expectedLen) return false;
  for (char c : s) {
    if (!isHexChar(c)) return false;
  }
  return true;
}

} // namespace

pp::Roe<std::string> readPrivateKey(const std::string &keyOrPath,
                                    const std::string &baseDir) {
  if (keyOrPath.empty()) {
    return Error(1, "Key path or value cannot be empty");
  }
  std::string resolvedPath = keyOrPath;
  if (!baseDir.empty()) {
    std::filesystem::path p(keyOrPath);
    if (p.is_relative()) {
      resolvedPath =
          (std::filesystem::path(baseDir) / p).lexically_normal().string();
    }
  }
  std::string content = readKey(resolvedPath);
  if (content.empty()) {
    return Error(2, "Failed to read key from: " + keyOrPath);
  }
  // Strip optional 0x prefix
  if (content.size() >= 2 && content[0] == '0' &&
      (content[1] == 'x' || content[1] == 'X')) {
    content = content.substr(2);
  }
  content = trimWhitespace(content);
  // Hex-encoded: 64 hex chars -> 32 bytes
  if (isHexString(content, 64)) {
    std::string raw = hexDecode(content);
    if (raw.size() != 32) {
      return Error(3, "Invalid hex-encoded private key (expected 64 hex chars)");
    }
    return raw;
  }
  // Raw 32 bytes
  if (content.size() == 32) {
    return content;
  }
  return Error(4, "Private key must be 32 bytes raw or 64 hex characters, got " +
                      std::to_string(content.size()));
}

} // namespace utl
} // namespace pp