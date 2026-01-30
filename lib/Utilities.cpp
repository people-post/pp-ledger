#include "Utilities.h"
#include <charconv>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {
namespace utl {

int64_t getCurrentTime() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
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
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
  if (!mdctx) {
    throw std::runtime_error("Failed to create EVP_MD_CTX");
  }

  const EVP_MD *md = EVP_sha256();
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hashLen = 0;

  if (EVP_DigestInit_ex(mdctx, md, nullptr) != 1) {
    EVP_MD_CTX_free(mdctx);
    throw std::runtime_error("EVP_DigestInit_ex failed");
  }

  if (EVP_DigestUpdate(mdctx, input.c_str(), input.size()) != 1) {
    EVP_MD_CTX_free(mdctx);
    throw std::runtime_error("EVP_DigestUpdate failed");
  }

  if (EVP_DigestFinal_ex(mdctx, hash, &hashLen) != 1) {
    EVP_MD_CTX_free(mdctx);
    throw std::runtime_error("EVP_DigestFinal_ex failed");
  }

  EVP_MD_CTX_free(mdctx);

  std::stringstream ss;
  for (unsigned int i = 0; i < hashLen; i++) {
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

} // namespace utl
} // namespace pp