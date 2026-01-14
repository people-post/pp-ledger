#include "BlockDir.h"
#include "Logger.h"
#include "../lib/Serializer.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace pp {

BlockDir::BlockDir()
    : Module("blockdir")
    , maxFileSize_(0)
    , currentFileId_(0)
    , managesBlockchain_(false) {
}

BlockDir::~BlockDir() {
    flush();
}

BlockDir::Roe<void> BlockDir::init(const Config& config, bool manageBlockchain) {
    dirPath_ = config.dirPath;
    maxFileSize_ = config.maxFileSize;
    currentFileId_ = 0;
    indexFilePath_ = dirPath_ + "/blocks.index";
    blockIndex_.clear();
    fileInfoMap_.clear();
    managesBlockchain_ = manageBlockchain;
    
    // Initialize blockchain if this BlockDir manages it
    if (managesBlockchain_) {
        ukpBlockchain_ = std::make_unique<BlockChain>();
    }
    
    // Create directory if it doesn't exist
    try {
        if (!std::filesystem::exists(dirPath_)) {
            std::filesystem::create_directories(dirPath_);
            log().info << "Created block directory: " << dirPath_;
        }
    } catch (const std::exception& e) {
        log().error << "Failed to create directory " << dirPath_ << ": " << e.what();
        return Error("Failed to create directory: " + std::string(e.what()));
    }
    
    // Load existing index if it exists
    if (std::filesystem::exists(indexFilePath_)) {
        if (!loadIndex()) {
            log().error << "Failed to load index file";
            return Error("Failed to load index file");
        }
        log().info << "Loaded index with " << fileInfoMap_.size() << " file ranges and " 
                   << blockIndex_.size() << " blocks";
        
        // Find the highest file ID from the file info map
        for (const auto& [fileId, _] : fileInfoMap_) {
            if (fileId > currentFileId_) {
                currentFileId_ = fileId;
            }
        }
    } else {
        log().info << "No existing index file, starting fresh";
    }
    
    // Open existing block files referenced in the file info map
    for (auto& [fileId, fileInfo] : fileInfoMap_) {
        std::string filepath = getBlockFilePath(fileId);
        if (std::filesystem::exists(filepath)) {
            auto ukpBlockFile = std::make_unique<BlockFile>();
            BlockFile::Config config(filepath, maxFileSize_);
            auto result = ukpBlockFile->init(config);
            if (result.isOk()) {
                fileInfo.blockFile = std::move(ukpBlockFile);
                log().debug << "Opened existing block file: " << filepath;
            } else {
                log().error << "Failed to open block file: " << filepath << ": " << result.error().message;
                return Error("Failed to open block file: " + filepath + ": " + result.error().message);
            }
        }
    }
    
    log().info << "BlockDir initialized with " << fileInfoMap_.size() 
               << " files and " << blockIndex_.size() << " blocks";
    
    // Populate blockchain from existing blocks if managing blockchain
    if (managesBlockchain_ && ukpBlockchain_ && !blockIndex_.empty()) {
        auto result = populateBlockchainFromStorage();
        if (result.isError()) {
            log().error << "Failed to populate blockchain from storage: " << result.error().message;
            return result.error();
        } else {
            log().info << "Populated blockchain with " << ukpBlockchain_->getSize() << " blocks from storage";
        }
    }
    
    return {};
}

BlockDir::Roe<void> BlockDir::writeBlock(uint64_t blockId, const void* data, uint64_t size) {
    // Check if block already exists
    if (hasBlock(blockId)) {
        log().warning << "Block " << blockId << " already exists, overwriting not supported";
        return Error("Block already exists");
    }
    
    // Get active block file for writing
    BlockFile* blockFile = getActiveBlockFile(size);
    if (!blockFile) {
        log().error << "Failed to get active block file";
        return Error("Failed to get active block file");
    }
    
    // Write data to the file
    auto result = blockFile->write(data, size);
    if (!result.isOk()) {
        log().error << "Failed to write block " << blockId << " to file";
        return Error("Failed to write block to file: " + result.error().message);
    }
    
    int64_t offset = result.value();
    
    // Update block index
    blockIndex_[blockId] = BlockLocation(currentFileId_, offset, size);
    
    // Update file info (block ID range)
    auto fileInfoIt = fileInfoMap_.find(currentFileId_);
    if (fileInfoIt != fileInfoMap_.end()) {
        // Add block location to the range
        if (fileInfoIt->second.blockRange.blockLocations.empty()) {
            // First block - set startBlockId
            fileInfoIt->second.blockRange.startBlockId = blockId;
        }
        fileInfoIt->second.blockRange.blockLocations.push_back(BlockOffsetSize(offset, size));
    } else {
        // This shouldn't happen if getActiveBlockFile works correctly
        log().warning << "File info not found for file " << currentFileId_;
    }
    
    log().debug << "Wrote block " << blockId << " to file " << currentFileId_ 
                << " at offset " << offset << " (size: " << size << " bytes)";
    
    // Save index after each write (for durability)
    saveIndex();
    
    return {};
}

bool BlockDir::hasBlock(uint64_t blockId) const {
    return blockIndex_.find(blockId) != blockIndex_.end();
}

void BlockDir::flush() {
    // BlockFile now flushes automatically after each write operation,
    // so no explicit flush needed here. Just save the index.
    
    // Save index
    if (!saveIndex()) {
        log().error << "Failed to save index during flush";
    }
}

BlockFile* BlockDir::createBlockFile(uint32_t fileId) {
    std::string filepath = getBlockFilePath(fileId);
    auto ukpBlockFile = std::make_unique<BlockFile>();
    
    BlockFile::Config config(filepath, maxFileSize_);
    auto result = ukpBlockFile->init(config);
    if (!result.isOk()) {
        log().error << "Failed to create block file: " << filepath;
        return nullptr;
    }
    
    log().info << "Created new block file: " << filepath;
    
    BlockFile* pBlockFile = ukpBlockFile.get();
    // Create FileInfo with empty range (will be updated when first block is written)
    FileInfo fileInfo;
    fileInfo.blockFile = std::move(ukpBlockFile);
    fileInfo.blockRange.startBlockId = 0;
    fileInfoMap_[fileId] = std::move(fileInfo);
    fileIdOrder_.push_back(fileId);  // Track file creation order
    return pBlockFile;
}

BlockFile* BlockDir::getActiveBlockFile(uint64_t dataSize) {
    // Check if current file exists and can fit the data
    auto it = fileInfoMap_.find(currentFileId_);
    if (it != fileInfoMap_.end() && it->second.blockFile && it->second.blockFile->canFit(dataSize)) {
        return it->second.blockFile.get();
    }
    
    // Need to create a new file
    currentFileId_++;
    return createBlockFile(currentFileId_);
}

BlockFile* BlockDir::getBlockFile(uint32_t fileId) {
    auto it = fileInfoMap_.find(fileId);
    if (it != fileInfoMap_.end() && it->second.blockFile) {
        return it->second.blockFile.get();
    }
    
    // Try to open the file if it exists
    std::string filepath = getBlockFilePath(fileId);
    if (std::filesystem::exists(filepath)) {
        auto ukpBlockFile = std::make_unique<BlockFile>();
        BlockFile::Config config(filepath, maxFileSize_);
        auto result = ukpBlockFile->init(config);
        if (result.isOk()) {
            BlockFile* pBlockFile = ukpBlockFile.get();
            // Create FileInfo with empty range (range should be loaded from index)
            // If range is not in map, it means this file wasn't in the index
            if (fileInfoMap_.find(fileId) == fileInfoMap_.end()) {
                FileInfo fileInfo;
                fileInfo.blockFile = std::move(ukpBlockFile);
                fileInfo.blockRange.startBlockId = 0;
                fileInfoMap_[fileId] = std::move(fileInfo);
            } else {
                fileInfoMap_[fileId].blockFile = std::move(ukpBlockFile);
            }
            return pBlockFile;
        }
    }
    
    return nullptr;
}

std::string BlockDir::getBlockFilePath(uint32_t fileId) const {
    std::ostringstream oss;
    oss << dirPath_ << "/block_" << std::setw(6) << std::setfill('0') << fileId << ".dat";
    return oss.str();
}

bool BlockDir::loadIndex() {
    std::ifstream indexFile(indexFilePath_, std::ios::binary);
    if (!indexFile.is_open()) {
        log().error << "Failed to open index file: " << indexFilePath_;
        return false;
    }
    
    fileInfoMap_.clear();
    blockIndex_.clear();
    
    // Read and validate header
    if (!readIndexHeader(indexFile)) {
        log().error << "Failed to read or validate index file header";
        indexFile.close();
        return false;
    }
    
    // Read index entries (ranges) using Archive utilities
    IndexEntry entry;
    while (indexFile.good() && !indexFile.eof()) {
        // Check if we're at end of file
        if (indexFile.peek() == EOF) break;
        
        // Deserialize using Archive
        if (!Serializer::deserializeFromStream(indexFile, entry)) {
            if (indexFile.gcount() == 0) break; // End of file
            log().warning << "Failed to read complete index entry";
            break;
        }
        
        // Store block locations directly in blockIndex_
        uint64_t endBlockId = entry.blockRange.startBlockId;
        for (size_t i = 0; i < entry.blockRange.blockLocations.size(); i++) {
            uint64_t blockId = entry.blockRange.startBlockId + i;
            const auto& blockLoc = entry.blockRange.blockLocations[i];
            blockIndex_[blockId] = BlockLocation(entry.fileId, blockLoc);
            endBlockId = blockId;
        }
        
        // Create FileInfo with empty BlockFile (will be loaded when needed)
        // Store file ID range
        FileInfo fileInfo;
        fileInfo.blockFile = nullptr;
        fileInfo.blockRange = entry.blockRange;
        fileInfoMap_[entry.fileId] = std::move(fileInfo);
    }
    
    indexFile.close();
    log().debug << "Loaded " << fileInfoMap_.size() << " file ranges and " 
                << blockIndex_.size() << " block entries from index";
    
    return true;
}

bool BlockDir::saveIndex() {
    std::ofstream indexFile(indexFilePath_, std::ios::binary | std::ios::trunc);
    if (!indexFile.is_open()) {
        log().error << "Failed to open index file for writing: " << indexFilePath_;
        return false;
    }
    
    // Write header first
    if (!writeIndexHeader(indexFile)) {
        log().error << "Failed to write index file header";
        indexFile.close();
        return false;
    }
    
    // Write index entries with block locations using Archive utilities
    for (const auto& [fileId, fileInfo] : fileInfoMap_) {
        uint64_t startBlockId = fileInfo.blockRange.startBlockId;
        IndexEntry entry(fileId, startBlockId);
        
        // Use block locations from fileInfo.blockRange if available
        if (!fileInfo.blockRange.blockLocations.empty()) {
            entry.blockRange.blockLocations = fileInfo.blockRange.blockLocations;
        } else {
            // Fallback: Collect all blocks for this file from blockIndex_ in sequential order
            // Since blockIds are sequential starting from startBlockId, iterate in order
            uint64_t endBlockId = startBlockId;
            if (!fileInfo.blockRange.blockLocations.empty()) {
                endBlockId = startBlockId + fileInfo.blockRange.blockLocations.size() - 1;
            }
            for (uint64_t blockId = startBlockId; blockId <= endBlockId; blockId++) {
                auto it = blockIndex_.find(blockId);
                if (it != blockIndex_.end() && it->second.fileId == fileId) {
                    entry.blockRange.blockLocations.push_back(it->second.offsetSize);
                }
            }
        }
        
        Serializer::serializeToStream(indexFile, entry);
    }
    
    indexFile.close();
    log().debug << "Saved " << fileInfoMap_.size() << " file ranges to index";
    
    return true;
}

bool BlockDir::writeIndexHeader(std::ostream& os) {
    IndexFileHeader header;
    Serializer::serializeToStream(os, header);
    
    if (!os.good()) {
        log().error << "Failed to write index file header";
        return false;
    }
    
    log().debug << "Wrote index file header (magic: 0x" << std::hex << header.magic 
                << std::dec << ", version: " << header.version << ")";
    
    return true;
}

bool BlockDir::readIndexHeader(std::istream& is) {
    IndexFileHeader header;
    
    if (!Serializer::deserializeFromStream(is, header)) {
        log().error << "Failed to read index file header";
        return false;
    }
    
    // Validate header
    if (header.magic != IndexFileHeader::MAGIC) {
        log().error << "Invalid magic number in index file header: 0x" 
                    << std::hex << header.magic << std::dec;
        return false;
    }
    
    if (header.version > IndexFileHeader::CURRENT_VERSION) {
        log().error << "Unsupported index file version " << header.version 
                    << " (current: " << IndexFileHeader::CURRENT_VERSION << ")";
        return false;
    }
    
    log().debug << "Read index file header (magic: 0x" << std::hex << header.magic 
                << std::dec << ", version: " << header.version << ")";
    
    return true;
}

uint32_t BlockDir::getFrontFileId() const {
    if (fileIdOrder_.empty()) {
        return 0;
    }
    return fileIdOrder_.front();
}

std::unique_ptr<BlockFile> BlockDir::popFrontFile() {
    if (fileIdOrder_.empty()) {
        log().warning << "No files to pop from BlockDir";
        return nullptr;
    }
    
    uint32_t frontFileId = fileIdOrder_.front();
    fileIdOrder_.erase(fileIdOrder_.begin());
    
    auto it = fileInfoMap_.find(frontFileId);
    if (it == fileInfoMap_.end() || !it->second.blockFile) {
        log().error << "Front file ID " << frontFileId << " not found in file map";
        return nullptr;
    }
    
    // Remove all blocks that belong to this file from the index
    std::vector<uint64_t> blocksToRemove;
    for (auto& indexEntry : blockIndex_) {
        if (indexEntry.second.fileId == frontFileId) {
            blocksToRemove.push_back(indexEntry.first);
        }
    }
    
    for (uint64_t blockId : blocksToRemove) {
        blockIndex_.erase(blockId);
    }
    
    // Extract and return the file, remove from fileInfoMap_
    std::unique_ptr<BlockFile> poppedFile = std::move(it->second.blockFile);
    fileInfoMap_.erase(it);
    
    log().info << "Popped front file " << frontFileId << " with " << blocksToRemove.size() << " blocks";
    return poppedFile;
}

BlockDir::Roe<void> BlockDir::moveFrontFileTo(BlockDir& targetDir) {
    uint32_t frontFileId = getFrontFileId();
    if (frontFileId == 0) {
        return Error("No files to move");
    }
    
    // Get file info from fileInfoMap_ before popping
    auto fileInfoIt = fileInfoMap_.find(frontFileId);
    if (fileInfoIt == fileInfoMap_.end()) {
        return Error("Front file not found in fileInfoMap_");
    }
    
    const FileInfo& fileInfo = fileInfoIt->second;
    const FileBlockRange& blockRange = fileInfo.blockRange;
    
    // Calculate block count for logging and blockchain trimming
    size_t blockCount = blockRange.blockLocations.size();
    
    // First, move fileInfo.blockRange to target directory's fileInfoMap_
    // Create FileInfo in target directory (BlockFile will be loaded when needed)
    if (targetDir.fileInfoMap_.find(frontFileId) == targetDir.fileInfoMap_.end()) {
        FileInfo targetFileInfo;
        targetFileInfo.blockFile = nullptr;
        targetFileInfo.blockRange = blockRange;
        targetDir.fileInfoMap_[frontFileId] = std::move(targetFileInfo);
    } else {
        return Error("Front file already exists in target directory");
    }    

    // Then, add items to block ID indexed maps (blockIndex_)
    uint64_t startBlockId = blockRange.startBlockId;
    for (size_t i = 0; i < blockRange.blockLocations.size(); i++) {
        uint64_t blockId = startBlockId + i;
        const BlockOffsetSize& offsetSize = blockRange.blockLocations[i];
        BlockLocation location(frontFileId, offsetSize);
        targetDir.blockIndex_[blockId] = location;
    }
    
    std::string sourceFilePath = getBlockFilePath(frontFileId);
    std::string targetFilePath = targetDir.getBlockFilePath(frontFileId);
    
    // Pop the file from this directory
    auto poppedFile = popFrontFile();
    if (!poppedFile) {
        return Error("Failed to pop front file");
    }
    
    // Release the file handle before moving
    poppedFile.reset();
    
    // Move file on disk
    try {
        std::filesystem::rename(sourceFilePath, targetFilePath);
    } catch (const std::exception& e) {
        return Error(std::string("Failed to move file: ") + e.what());
    }
    
    // Update target directory's file tracking if needed
    // Add file ID to target's tracking if not already present
    auto it = std::find(targetDir.fileIdOrder_.begin(), targetDir.fileIdOrder_.end(), frontFileId);
    if (it == targetDir.fileIdOrder_.end()) {
        targetDir.fileIdOrder_.push_back(frontFileId);
    }
    
    // Automatically trim blocks from blockchain if this BlockDir manages a blockchain
    if (managesBlockchain_ && ukpBlockchain_) {
        size_t removed = trimBlocks(blockCount);
        if (removed > 0) {
            log().info << "Automatically trimmed " << removed << " blocks from blockchain after moving to archive";
        }
    }
    
    log().info << "Moved front file " << frontFileId << " with " << blockCount << " blocks to target directory";
    return {};
}

size_t BlockDir::getTotalStorageSize() const {
    size_t totalSize = 0;
    for (const auto& [fileId, fileInfo] : fileInfoMap_) {
        if (fileInfo.blockFile) {
            // File is open, use the open file's size
            totalSize += fileInfo.blockFile->getCurrentSize();
        } else {
            // File is not open, check file size on disk
            std::string filepath = getBlockFilePath(fileId);
            if (std::filesystem::exists(filepath)) {
                try {
                    totalSize += std::filesystem::file_size(filepath);
                } catch (const std::exception&) {
                    // Skip this file if we can't get its size
                    // (file might have been deleted or is inaccessible)
                }
            }
        }
    }
    return totalSize;
}

// Blockchain management methods
bool BlockDir::addBlock(std::shared_ptr<Block> block) {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return false;
    }
    
    // Assume block index is always increment of 1 from last block
    // Set the block index to the current blockchain size (which will be the next index)
    uint64_t nextIndex = ukpBlockchain_->getSize();
    block->setIndex(nextIndex);
    
    // Add block to in-memory blockchain
    if (!ukpBlockchain_->addBlock(block)) {
        return false;
    }
    
    // Automatically write block to storage
    // Using block index as the block ID for storage
    // Serialize the full block using ltsToString() for long-term storage
    std::string blockData = block->ltsToString();
    auto writeResult = writeBlock(block->getIndex(), blockData.data(), blockData.size());
    if (!writeResult.isOk()) {
        log().error << "Failed to write block " << block->getIndex() << " to storage: " << writeResult.error().message;
        // Note: We don't rollback the blockchain addition, as the block is already in memory
        // In a production system, you might want to handle this differently
        return false;
    }
    
    // Automatically flush after adding block
    flush();
    
    return true;
}

std::shared_ptr<Block> BlockDir::getLatestBlock() const {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return nullptr;
    }
    return ukpBlockchain_->getLatestBlock();
}

size_t BlockDir::getBlockchainSize() const {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return 0;
    }
    return ukpBlockchain_->getSize();
}

std::shared_ptr<Block> BlockDir::getBlock(uint64_t index) const {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return nullptr;
    }
    return ukpBlockchain_->getBlock(index);
}

bool BlockDir::isBlockchainValid() const {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return false;
    }
    return ukpBlockchain_->isValid();
}

std::string BlockDir::getLastBlockHash() const {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return "0";
    }
    return ukpBlockchain_->getLastBlockHash();
}

size_t BlockDir::trimBlocks(size_t count) {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return 0;
    }
    return ukpBlockchain_->trimBlocks(count);
}

BlockDir::Roe<size_t> BlockDir::loadBlocksFromFile(uint32_t fileId) {
    const auto& fileInfo = fileInfoMap_[fileId];
    const FileBlockRange& blockRange = fileInfo.blockRange;
    uint64_t startBlockId = blockRange.startBlockId;
    
    // Get the block file
    BlockFile* blockFile = getBlockFile(fileId);
    if (!blockFile) {
        uint64_t endBlockId = startBlockId;
        if (!blockRange.blockLocations.empty()) {
            endBlockId = startBlockId + blockRange.blockLocations.size() - 1;
        }
        log().error << "Failed to get block file " << fileId << " for range " 
                    << startBlockId << "-" << endBlockId;
        return Error("Failed to get block file " + std::to_string(fileId));
    }
    
    size_t loadedCount = 0;
    
    // Iterate through block locations from fileInfoMap_
    for (size_t i = 0; i < blockRange.blockLocations.size(); i++) {
        uint64_t blockId = startBlockId + i;
        const BlockOffsetSize& offsetSize = blockRange.blockLocations[i];
        
        // Read block data from file using offset and size from fileInfoMap_
        std::string blockData;
        blockData.resize(static_cast<size_t>(offsetSize.size));
        auto readResult = blockFile->read(offsetSize.offset, &blockData[0], offsetSize.size);
        if (!readResult.isOk() || readResult.value() != static_cast<int64_t>(offsetSize.size)) {
            log().error << "Failed to read block " << blockId << " from file " << fileId;
            return Error("Failed to read block " + std::to_string(blockId) + " from file " + std::to_string(fileId));
        }
        
        // Deserialize block from binary format
        auto block = std::make_shared<Block>();
        if (!block->ltsFromString(blockData)) {
            log().error << "Failed to deserialize block " << blockId << " from storage";
            return Error("Failed to deserialize block " + std::to_string(blockId) + " from storage");
        }
        
        // Verify block index matches blockId
        if (block->getIndex() != blockId) {
            log().warning << "Block index mismatch: expected " << blockId 
                          << ", got " << block->getIndex();
            return Error("Block index mismatch: expected " + std::to_string(blockId) + ", got " + std::to_string(block->getIndex()));
        }
        
        // Add to blockchain
        // Note: We don't use addBlock() here because it would try to write to storage again
        // Instead, we directly add to the blockchain
        if (!ukpBlockchain_->addBlock(block)) {
            log().error << "Failed to add block " << blockId << " to blockchain";
            return Error("Failed to add block " + std::to_string(blockId) + " to blockchain");
        }
        
        loadedCount++;
    }
    
    return loadedCount;
}

BlockDir::Roe<void> BlockDir::populateBlockchainFromStorage() {
    if (!managesBlockchain_ || !ukpBlockchain_) {
        return Error("Blockchain management not enabled or blockchain not initialized");
    }
    
    // Iterate through files and their block ID ranges
    // Collect file IDs and sort them by startBlockId to maintain sequential order
    std::vector<std::pair<uint32_t, uint64_t>> fileIdWithStart;
    for (const auto& [fileId, fileInfo] : fileInfoMap_) {
        fileIdWithStart.push_back(std::make_pair(fileId, fileInfo.blockRange.startBlockId));
    }
    // Sort by startBlockId to process files in sequential order
    std::sort(fileIdWithStart.begin(), fileIdWithStart.end(), 
              [](const auto& a, const auto& b) { return a.second < b.second; });
    
    std::vector<uint32_t> fileIds;
    for (const auto& [fileId, _] : fileIdWithStart) {
        fileIds.push_back(fileId);
    }
    
    size_t loadedCount = 0;
    
    // Process each file in order
    for (uint32_t fileId : fileIds) {
        auto result = loadBlocksFromFile(fileId);
        if (!result.isOk()) {
            log().error << "Failed to load blocks from file " << fileId << ": " << result.error().message;
            return result.error();
        }
        loadedCount += result.value();
    }
    
    log().debug << "Loaded " << loadedCount << " blocks from storage into blockchain";
    if (loadedCount == 0) {
        return Error("No blocks were loaded from storage");
    }
    
    return {};
}

} // namespace pp
