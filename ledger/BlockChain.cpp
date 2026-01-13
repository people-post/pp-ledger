#include "BlockChain.h"
#include "Block.h"

#include <stdexcept>
#include <algorithm>
#include <openssl/evp.h>

namespace pp {

// BlockChain implementation
BlockChain::BlockChain()
    : Module("blockchain"), 
      difficulty_(2),
      maxActiveDirSize_(500 * 1024 * 1024) {
    createGenesisBlock();
}

bool BlockChain::initStorage(const StorageConfig& config) {
    try {
        // Initialize active BlockDir
        activeBlockDir_ = std::make_unique<BlockDir>();
        BlockDir::Config activeCfg(config.activeDirPath, config.blockDirFileSize);
        auto activeRes = activeBlockDir_->init(activeCfg);
        if (!activeRes) {
            logging::getLogger("blockchain").error << "Failed to initialize active BlockDir";
            return false;
        }
        
        // Initialize archive BlockDir
        archiveBlockDir_ = std::make_unique<BlockDir>();
        BlockDir::Config archiveCfg(config.archiveDirPath, config.blockDirFileSize);
        auto archiveRes = archiveBlockDir_->init(archiveCfg);
        if (!archiveRes) {
            logging::getLogger("blockchain").error << "Failed to initialize archive BlockDir";
            return false;
        }
        
        maxActiveDirSize_ = config.maxActiveDirSize;
        return true;
    } catch (const std::exception& e) {
        logging::getLogger("blockchain").error << "Failed to initialize storage: " << e.what();
        return false;
    }
}

void BlockChain::createGenesisBlock() {
    auto genesis = std::make_shared<Block>();
    genesis->setIndex(0);
    genesis->setData("Genesis Block");
    genesis->setPreviousHash("0");
    genesis->setHash(genesis->calculateHash());
    genesis->mineBlock(difficulty_);
    chain_.push_back(genesis);
}

void BlockChain::transferBlocksToArchive() {
    if (!activeBlockDir_ || !archiveBlockDir_) {
        return; // Storage not initialized
    }
    
    // Transfer files from active to archive based on storage usage
    while (activeBlockDir_->getTotalStorageSize() >= maxActiveDirSize_) {
        auto result = activeBlockDir_->moveFrontFileTo(*archiveBlockDir_);
        if (!result.isOk()) {
            logging::getLogger("blockchain").error << "Failed to move front file to archive";
            break;
        }
        
        // Flush to persist changes
        activeBlockDir_->flush();
        archiveBlockDir_->flush();
    }
}

// IBlockChain interface implementation
bool BlockChain::addBlock(std::shared_ptr<IBlock> block) {
    if (!block) {
        return false;
    }
    
    chain_.push_back(block);
    
    // Write to active storage if initialized
    if (activeBlockDir_) {
        // Serialize block and write to active directory
        // Using block index as the block ID for storage
        std::string blockData = block->getData();
        activeBlockDir_->writeBlock(
            block->getIndex(),
            blockData.data(),
            blockData.size()
        );
        
        // Check if we should transfer blocks to archive
        transferBlocksToArchive();
    }
    
    return true;
}

std::shared_ptr<IBlock> BlockChain::getLatestBlock() const {
    if (chain_.empty()) {
        return nullptr;
    }
    return chain_.back();
}

std::shared_ptr<IBlock> BlockChain::getBlock(uint64_t index) const {
    if (index >= chain_.size()) {
        return nullptr;
    }
    return chain_[index];
}

size_t BlockChain::getSize() const {
    return chain_.size();
}

bool BlockChain::isValid() const {
    if (chain_.empty()) {
        return false;
    }
    
    // Start from index 1 (skip genesis block)
    for (size_t i = 1; i < chain_.size(); i++) {
        const auto& currentBlock = chain_[i];
        const auto& previousBlock = chain_[i - 1];
        
        // Verify current block's hash
        if (currentBlock->getHash() != currentBlock->calculateHash()) {
            return false;
        }
        
        // Verify link to previous block
        if (currentBlock->getPreviousHash() != previousBlock->getHash()) {
            return false;
        }
        
        // Verify proof of work
        std::string target(difficulty_, '0');
        if (currentBlock->getHash().substr(0, difficulty_) != target) {
            return false;
        }
    }
    
    return true;
}

bool BlockChain::validateBlock(const IBlock& block) const {
    // Verify block's hash
    if (block.getHash() != block.calculateHash()) {
        return false;
    }
    
    // Verify proof of work
    std::string target(difficulty_, '0');
    if (block.getHash().substr(0, difficulty_) != target) {
        return false;
    }
    
    return true;
}

std::vector<std::shared_ptr<IBlock>> BlockChain::getBlocks(uint64_t fromIndex, uint64_t toIndex) const {
    std::vector<std::shared_ptr<IBlock>> result;
    
    if (fromIndex > toIndex || fromIndex >= chain_.size()) {
        return result;
    }
    
    uint64_t endIndex = std::min(toIndex + 1, static_cast<uint64_t>(chain_.size()));
    for (uint64_t i = fromIndex; i < endIndex; i++) {
        result.push_back(chain_[i]);
    }
    
    return result;
}

std::string BlockChain::getLastBlockHash() const {
    if (chain_.empty()) {
        return "0";
    }
    return chain_.back()->getHash();
}

void BlockChain::setDifficulty(uint32_t difficulty) {
    difficulty_ = difficulty;
}

uint32_t BlockChain::getDifficulty() const {
    return difficulty_;
}

} // namespace pp
