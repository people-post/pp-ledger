#include "Block.h"

#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <chrono>
#include <openssl/evp.h>

namespace pp {

// Helper function to compute SHA-256 hash using OpenSSL 3.0 EVP API
static std::string sha256(const std::string& input) {
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        throw std::runtime_error("Failed to create EVP_MD_CTX");
    }
    
    const EVP_MD* md = EVP_sha256();
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
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

// Block implementation
Block::Block()
    : index_(0),
      timestamp_(std::chrono::system_clock::now().time_since_epoch().count()),
      data_(""),
      previousHash_(""),
      nonce_(0),
      slot_(0),
      slotLeader_("") {
    hash_ = calculateHash();
}

// IBlock interface implementation
uint64_t Block::getIndex() const {
    return index_;
}

int64_t Block::getTimestamp() const {
    return timestamp_;
}

std::string Block::getData() const {
    return data_;
}

std::string Block::getPreviousHash() const {
    return previousHash_;
}

std::string Block::getHash() const {
    return hash_;
}

uint64_t Block::getNonce() const {
    return nonce_;
}

std::string Block::calculateHash() const {
    std::stringstream ss;
    ss << CURRENT_VERSION << index_ << timestamp_ << data_ << previousHash_ << nonce_;
    return sha256(ss.str());
}

uint64_t Block::getSlot() const {
    return slot_;
}

std::string Block::getSlotLeader() const {
    return slotLeader_;
}

void Block::setHash(const std::string& hash) {
    hash_ = hash;
}

void Block::setNonce(uint64_t nonce) {
    nonce_ = nonce;
}

// Additional setters
void Block::setIndex(uint64_t index) {
    index_ = index;
}

void Block::setTimestamp(int64_t timestamp) {
    timestamp_ = timestamp;
}

void Block::setData(const std::string& data) {
    data_ = data;
}

void Block::setPreviousHash(const std::string& hash) {
    previousHash_ = hash;
}

void Block::setSlot(uint64_t slot) {
    slot_ = slot;
}

void Block::setSlotLeader(const std::string& leader) {
    slotLeader_ = leader;
}

uint16_t Block::getVersion() const {
    return CURRENT_VERSION;
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
    oss.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    // Index (uint64_t)
    oss.write(reinterpret_cast<const char*>(&index_), sizeof(index_));
    
    // Timestamp (int64_t)
    oss.write(reinterpret_cast<const char*>(&timestamp_), sizeof(timestamp_));
    
    // Data: size + content
    uint64_t dataSize = data_.size();
    oss.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    if (dataSize > 0) {
        oss.write(data_.data(), dataSize);
    }
    
    // Previous hash: size + content
    uint64_t prevHashSize = previousHash_.size();
    oss.write(reinterpret_cast<const char*>(&prevHashSize), sizeof(prevHashSize));
    if (prevHashSize > 0) {
        oss.write(previousHash_.data(), prevHashSize);
    }
    
    // Hash: size + content
    uint64_t hashSize = hash_.size();
    oss.write(reinterpret_cast<const char*>(&hashSize), sizeof(hashSize));
    if (hashSize > 0) {
        oss.write(hash_.data(), hashSize);
    }
    
    // Nonce (uint64_t)
    oss.write(reinterpret_cast<const char*>(&nonce_), sizeof(nonce_));
    
    // Slot (uint64_t)
    oss.write(reinterpret_cast<const char*>(&slot_), sizeof(slot_));
    
    // Slot leader: size + content
    uint64_t leaderSize = slotLeader_.size();
    oss.write(reinterpret_cast<const char*>(&leaderSize), sizeof(leaderSize));
    if (leaderSize > 0) {
        oss.write(slotLeader_.data(), leaderSize);
    }
    
    return oss.str();
}

bool Block::ltsFromString(const std::string& str) {
    try {
        std::istringstream iss(str, std::ios::binary);
        
        // Reset to default values
        index_ = 0;
        timestamp_ = 0;
        data_.clear();
        previousHash_.clear();
        hash_.clear();
        nonce_ = 0;
        slot_ = 0;
        slotLeader_.clear();
        
        // Read version (uint16_t) - read but don't store, validate compatibility
        uint16_t version = 0;
        if (!iss.read(reinterpret_cast<char*>(&version), sizeof(version))) {
            return false;
        }
        // Validate version compatibility (can read current and future versions up to a limit)
        if (version > CURRENT_VERSION) {
            return false; // Unsupported future version
        }
        
        // Read index (uint64_t)
        if (!iss.read(reinterpret_cast<char*>(&index_), sizeof(index_))) {
            return false;
        }
        
        // Read timestamp (int64_t)
        if (!iss.read(reinterpret_cast<char*>(&timestamp_), sizeof(timestamp_))) {
            return false;
        }
        
        // Read data: size + content
        uint64_t dataSize = 0;
        if (!iss.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize))) {
            return false;
        }
        if (dataSize > 0) {
            data_.resize(dataSize);
            if (!iss.read(&data_[0], dataSize)) {
                return false;
            }
        }
        
        // Read previous hash: size + content
        uint64_t prevHashSize = 0;
        if (!iss.read(reinterpret_cast<char*>(&prevHashSize), sizeof(prevHashSize))) {
            return false;
        }
        if (prevHashSize > 0) {
            previousHash_.resize(prevHashSize);
            if (!iss.read(&previousHash_[0], prevHashSize)) {
                return false;
            }
        }
        
        // Read hash: size + content
        uint64_t hashSize = 0;
        if (!iss.read(reinterpret_cast<char*>(&hashSize), sizeof(hashSize))) {
            return false;
        }
        if (hashSize > 0) {
            hash_.resize(hashSize);
            if (!iss.read(&hash_[0], hashSize)) {
                return false;
            }
        }
        
        // Read nonce (uint64_t)
        if (!iss.read(reinterpret_cast<char*>(&nonce_), sizeof(nonce_))) {
            return false;
        }
        
        // Read slot (uint64_t)
        if (!iss.read(reinterpret_cast<char*>(&slot_), sizeof(slot_))) {
            return false;
        }
        
        // Read slot leader: size + content
        uint64_t leaderSize = 0;
        if (!iss.read(reinterpret_cast<char*>(&leaderSize), sizeof(leaderSize))) {
            return false;
        }
        if (leaderSize > 0) {
            slotLeader_.resize(leaderSize);
            if (!iss.read(&slotLeader_[0], leaderSize)) {
                return false;
            }
        }
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace pp
