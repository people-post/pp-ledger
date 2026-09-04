#include "Utilities.h"

#include "common/io/Json.h"
#include "crypto/MlDsa.h"
#include "crypto/Types.h"

#include <charconv>
#include <chrono>
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

  ByteVector toBytes(const std::string &s) {
    return ByteVector(reinterpret_cast<const uint8_t *>(s.data()),
                      reinterpret_cast<const uint8_t *>(s.data()) + s.size());
  }

  std::string fromBytes(const ByteVector &v) {
    return std::string(reinterpret_cast<const char *>(v.data()), v.size());
  }
}

int64_t getCurrentTime() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string formatTimestampLocal(int64_t unixSeconds) {
  time_t t = static_cast<time_t>(unixSeconds);
  std::tm local {};
#if defined(_WIN32)
  if (localtime_s(&local, &t) != 0) {
    return std::to_string(unixSeconds);
  }
#else
  if (localtime_r(&t, &local) == nullptr) {
    return std::to_string(unixSeconds);
  }
#endif
  char buf[64];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local) == 0)
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

pp::Roe<pp::common::Object> loadJsonFile(const std::string &configPath) {
  if (!std::filesystem::exists(configPath)) {
    return Error(1, "Configuration file not found: " + configPath);
  }

  std::ifstream configFile(configPath);
  if (!configFile.is_open()) {
    return Error(2, "Failed to open configuration file: " + configPath);
  }

  std::string content((std::istreambuf_iterator<char>(configFile)),
                      std::istreambuf_iterator<char>());
  configFile.close();

  auto parsed = pp::common::io::valueFromJsonString(content);
  if (!parsed.isOk()) {
    return Error(3, "Failed to parse JSON: " + parsed.error().message);
  }
  const auto *obj = pp::common::asObject(parsed.value());
  if (!obj) {
    return Error(3, "Configuration root must be a JSON object");
  }
  return *obj;
}

pp::Roe<pp::common::Object> parseJsonRequest(const std::string &request) {
  auto parsed = pp::common::io::valueFromJsonString(request);
  if (!parsed.isOk()) {
    return Error(1, "Failed to parse request JSON: " + parsed.error().message);
  }
  const auto *obj = pp::common::asObject(parsed.value());
  if (!obj) {
    return Error(2, "request root must be a JSON object");
  }
  if (!obj->contains("type")) {
    return Error(2, "missing type field");
  }
  return *obj;
}

std::string sha256(const std::string &input) {
  return hexEncode(sha256Raw(input));
}

std::string sha256Raw(const std::string &input) {
  unsigned char hash[crypto_hash_sha256_BYTES];

  if (crypto_hash_sha256(hash,
                         reinterpret_cast<const unsigned char*>(input.c_str()),
                         input.size()) != 0) {
    throw std::runtime_error("crypto_hash_sha256 failed");
  }

  return std::string(reinterpret_cast<const char *>(hash),
                     crypto_hash_sha256_BYTES);
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
  if (std::filesystem::exists(filePath)) {
    return Error(1, "File already exists: " + filePath);
  }

  std::filesystem::path path(filePath);
  std::filesystem::path parentDir = path.parent_path();
  if (!parentDir.empty() && !std::filesystem::exists(parentDir)) {
    std::error_code ec;
    std::filesystem::create_directories(parentDir, ec);
    if (ec) {
      return Error(2, "Failed to create parent directories for " + filePath + ": " + ec.message());
    }
  }

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

// --- ML-DSA-65

pp::common::Meta MlDsaKeyPair::ltsToMeta() const {
  pp::common::Meta m;
  m.set("publicKey", hexEncode(publicKey));
  m.set("privateKey", hexEncode(privateKey));
  return m;
}

pp::Roe<MlDsaKeyPair> mlDsaGenerate() {
  auto result = MlDsa::GenerateKeyPair();
  if (result.isError()) {
    return Error(1, result.error().message.empty() ? "ML-DSA-65 keygen failed"
                                                   : result.error().message);
  }
  MlDsaKeyPair pair;
  pair.publicKey = fromBytes(result->public_key);
  pair.privateKey = fromBytes(result->secret_key);
  return pair;
}

pp::Roe<std::string> mlDsaSign(const std::string &privateKey,
                               const std::string &message) {
  if (privateKey.size() != kMlDsaPrivateKeyBytes) {
    return Error(1, "mlDsaSign: private key must be " +
                        std::to_string(kMlDsaPrivateKeyBytes) + " bytes");
  }
  auto result = MlDsa::Sign(toBytes(privateKey), toBytes(message));
  if (result.isError()) {
    return Error(1, result.error().message.empty() ? "ML-DSA-65 sign failed"
                                                   : result.error().message);
  }
  return fromBytes(*result);
}

bool mlDsaVerify(const std::string &publicKey, const std::string &message,
                 const std::string &signature) {
  if (publicKey.size() != kMlDsaPublicKeyBytes ||
      signature.size() != kMlDsaSignatureBytes) {
    return false;
  }
  auto result =
      MlDsa::Verify(toBytes(publicKey), toBytes(message), toBytes(signature));
  if (result.isError()) {
    return false;
  }
  return *result;
}

bool isValidMlDsaPublicKey(const std::string &str) {
  std::string raw;
  if (str.size() == kMlDsaPublicKeyBytes) {
    raw = str;
  } else if (str.size() == kMlDsaPublicKeyBytes * 2) {
    raw = hexDecode(str);
    if (raw.size() != kMlDsaPublicKeyBytes) {
      return false;
    }
  } else if (str.size() == kMlDsaPublicKeyBytes * 2 + 2 &&
             (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))) {
    raw = hexDecode(str.substr(2));
    if (raw.size() != kMlDsaPublicKeyBytes) {
      return false;
    }
  } else {
    return false;
  }
  for (unsigned char c : raw) {
    if (c != 0) {
      return true;
    }
  }
  return false;
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
  if (content.size() >= 2 && content[0] == '0' &&
      (content[1] == 'x' || content[1] == 'X')) {
    content = content.substr(2);
  }
  content = trimWhitespace(content);
  const size_t hexLen = kMlDsaPrivateKeyBytes * 2;
  if (isHexString(content, hexLen)) {
    std::string raw = hexDecode(content);
    if (raw.size() != kMlDsaPrivateKeyBytes) {
      return Error(3, "Invalid hex-encoded private key (expected " +
                          std::to_string(hexLen) + " hex chars)");
    }
    return raw;
  }
  if (content.size() == kMlDsaPrivateKeyBytes) {
    return content;
  }
  return Error(4, "Private key must be " + std::to_string(kMlDsaPrivateKeyBytes) +
                      " bytes raw or " + std::to_string(hexLen) +
                      " hex characters, got " + std::to_string(content.size()));
}

} // namespace utl
} // namespace pp
