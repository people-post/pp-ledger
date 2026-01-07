#include "BlockChain.h"

#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <openssl/sha.h>

namespace pp {

// Helper function to compute SHA-256 hash
static std::string sha256(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, input.c_str(), input.size());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

// Block implementation
Block::Block(uint64_t idx, const std::string& blockData, const std::string& prevHash)
    : index(idx),
      timestamp(std::chrono::system_clock::now().time_since_epoch().count()),
      data(blockData),
      previousHash(prevHash),
      nonce(0) {
    hash = calculateHash();
}

std::string Block::calculateHash() const {
    std::stringstream ss;
    ss << index << timestamp << data << previousHash << nonce;
    return sha256(ss.str());
}

void Block::mineBlock(uint32_t difficulty) {
    std::string target(difficulty, '0');
    
    while (hash.substr(0, difficulty) != target) {
        nonce++;
        hash = calculateHash();
    }
}

// BlockChain implementation
BlockChain::BlockChain(uint32_t difficulty)
    : difficulty_(difficulty) {
    createGenesisBlock();
}

void BlockChain::createGenesisBlock() {
    Block genesis(0, "Genesis Block", "0");
    genesis.mineBlock(difficulty_);
    chain_.push_back(genesis);
}

void BlockChain::addBlock(const std::string& data) {
    if (chain_.empty()) {
        throw std::runtime_error("Chain is empty, cannot add block");
    }
    
    Block newBlock(chain_.size(), data, getLastBlockHash());
    newBlock.mineBlock(difficulty_);
    chain_.push_back(newBlock);
}

const std::vector<Block>& BlockChain::getChain() const {
    return chain_;
}

bool BlockChain::isValid() const {
    if (chain_.empty()) {
        return false;
    }
    
    // Start from index 1 (skip genesis block)
    for (size_t i = 1; i < chain_.size(); i++) {
        const Block& currentBlock = chain_[i];
        const Block& previousBlock = chain_[i - 1];
        
        // Verify current block's hash
        if (currentBlock.hash != currentBlock.calculateHash()) {
            return false;
        }
        
        // Verify link to previous block
        if (currentBlock.previousHash != previousBlock.hash) {
            return false;
        }
        
        // Verify proof of work
        std::string target(difficulty_, '0');
        if (currentBlock.hash.substr(0, difficulty_) != target) {
            return false;
        }
    }
    
    return true;
}

size_t BlockChain::getSize() const {
    return chain_.size();
}

const Block& BlockChain::getLatestBlock() const {
    if (chain_.empty()) {
        throw std::runtime_error("Chain is empty");
    }
    return chain_.back();
}

const Block& BlockChain::getBlock(size_t index) const {
    if (index >= chain_.size()) {
        throw std::out_of_range("Block index out of range");
    }
    return chain_[index];
}

void BlockChain::setDifficulty(uint32_t difficulty) {
    difficulty_ = difficulty;
}

uint32_t BlockChain::getDifficulty() const {
    return difficulty_;
}

std::string BlockChain::getLastBlockHash() const {
    if (chain_.empty()) {
        return "0";
    }
    return chain_.back().hash;
}

} // namespace pp
