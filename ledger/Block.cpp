#include "Block.h"

#include <chrono>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>

namespace pp {

// Helper function to compute SHA-256 hash using OpenSSL 3.0 EVP API
static std::string sha256(const std::string &input) {
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

// Block implementation
Block::Block()
    : index(0),
      timestamp(std::chrono::system_clock::now().time_since_epoch().count()),
      data(""), previousHash(""), nonce(0), slot(0), slotLeader("") {
  hash = calculateHash();
}

std::string Block::calculateHash() const {
  std::stringstream ss;
  ss << CURRENT_VERSION << index << timestamp << data << previousHash
     << nonce;
  return sha256(ss.str());
}

std::string Block::ltsToString() const {
  std::ostringstream oss(std::ios::binary);

  // Binary format for long-term storage:
  // [version (2 bytes)][index (8 bytes)][timestamp (8 bytes)]
  // [data_size (8 bytes)][data (data_size bytes)]
  // [previousHash_size (8 bytes)][previousHash (previousHash_size bytes)]
  // [hash_size (8 bytes)][hash (hash_size bytes)]
  // [nonce (8 bytes)][slot (8 bytes)]
  // [slotLeader_size (8 bytes)][slotLeader (slotLeader_size bytes)]

  // Version (uint16_t) - write CURRENT_VERSION
  uint16_t version = CURRENT_VERSION;
  oss.write(reinterpret_cast<const char *>(&version), sizeof(version));

  // Index (uint64_t)
  oss.write(reinterpret_cast<const char *>(&index), sizeof(index));

  // Timestamp (int64_t)
  oss.write(reinterpret_cast<const char *>(&timestamp), sizeof(timestamp));

  // Data: size + content
  uint64_t dataSize = data.size();
  oss.write(reinterpret_cast<const char *>(&dataSize), sizeof(dataSize));
  if (dataSize > 0) {
    oss.write(data.data(), dataSize);
  }

  // Previous hash: size + content
  uint64_t prevHashSize = previousHash.size();
  oss.write(reinterpret_cast<const char *>(&prevHashSize),
            sizeof(prevHashSize));
  if (prevHashSize > 0) {
    oss.write(previousHash.data(), prevHashSize);
  }

  // Hash: size + content
  uint64_t hashSize = hash.size();
  oss.write(reinterpret_cast<const char *>(&hashSize), sizeof(hashSize));
  if (hashSize > 0) {
    oss.write(hash.data(), hashSize);
  }

  // Nonce (uint64_t)
  oss.write(reinterpret_cast<const char *>(&nonce), sizeof(nonce));

  // Slot (uint64_t)
  oss.write(reinterpret_cast<const char *>(&slot), sizeof(slot));

  // Slot leader: size + content
  uint64_t leaderSize = slotLeader.size();
  oss.write(reinterpret_cast<const char *>(&leaderSize), sizeof(leaderSize));
  if (leaderSize > 0) {
    oss.write(slotLeader.data(), leaderSize);
  }

  return oss.str();
}

bool Block::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);

  // Reset to default values
  index = 0;
  timestamp = 0;
  data.clear();
  previousHash.clear();
  hash.clear();
  nonce = 0;
  slot = 0;
  slotLeader.clear();

  // Read version (uint16_t) - read but don't store, validate compatibility
  uint16_t version = 0;
  if (!iss.read(reinterpret_cast<char *>(&version), sizeof(version))) {
    return false;
  }
  // Validate version compatibility (can read current and future versions up
  // to a limit)
  if (version > CURRENT_VERSION) {
    return false; // Unsupported future version
  }

  // Read index (uint64_t)
  if (!iss.read(reinterpret_cast<char *>(&index), sizeof(index))) {
    return false;
  }

  // Read timestamp (int64_t)
  if (!iss.read(reinterpret_cast<char *>(&timestamp), sizeof(timestamp))) {
    return false;
  }

  // Read data: size + content
  uint64_t dataSize = 0;
  if (!iss.read(reinterpret_cast<char *>(&dataSize), sizeof(dataSize))) {
    return false;
  }
  if (dataSize > 0) {
    data.resize(dataSize);
    if (!iss.read(&data[0], dataSize)) {
      return false;
    }
  }

  // Read previous hash: size + content
  uint64_t prevHashSize = 0;
  if (!iss.read(reinterpret_cast<char *>(&prevHashSize),
                sizeof(prevHashSize))) {
    return false;
  }
  if (prevHashSize > 0) {
    previousHash.resize(prevHashSize);
    if (!iss.read(&previousHash[0], prevHashSize)) {
      return false;
    }
  }

  // Read hash: size + content
  uint64_t hashSize = 0;
  if (!iss.read(reinterpret_cast<char *>(&hashSize), sizeof(hashSize))) {
    return false;
  }
  if (hashSize > 0) {
    hash.resize(hashSize);
    if (!iss.read(&hash[0], hashSize)) {
      return false;
    }
  }

  // Read nonce (uint64_t)
  if (!iss.read(reinterpret_cast<char *>(&nonce), sizeof(nonce))) {
    return false;
  }

  // Read slot (uint64_t)
  if (!iss.read(reinterpret_cast<char *>(&slot), sizeof(slot))) {
    return false;
  }

  // Read slot leader: size + content
  uint64_t leaderSize = 0;
  if (!iss.read(reinterpret_cast<char *>(&leaderSize), sizeof(leaderSize))) {
    return false;
  }
  if (leaderSize > 0) {
    slotLeader.resize(leaderSize);
    if (!iss.read(&slotLeader[0], leaderSize)) {
      return false;
    }
  }

  return true;
}

} // namespace pp
