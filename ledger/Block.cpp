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
    ss << index_ << timestamp_ << data_ << previousHash_ << nonce_;
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

} // namespace pp
