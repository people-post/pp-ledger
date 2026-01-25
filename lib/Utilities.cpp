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

} // namespace utl
} // namespace pp