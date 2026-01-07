#include "BlockDir.h"
#include "Logger.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace pp {

BlockDir::BlockDir()
    : Module("blockdir")
    , maxFileSize_(0)
    , currentFileId_(0) {
}

BlockDir::~BlockDir() {
    flush();
}

bool BlockDir::init(const Config& config) {
    dirPath_ = config.dirPath;
    maxFileSize_ = config.maxFileSize;
    currentFileId_ = 0;
    indexFilePath_ = dirPath_ + "/blocks.index";
    blockIndex_.clear();
    ukpBlockFiles_.clear();
    
    // Create directory if it doesn't exist
    try {
        if (!std::filesystem::exists(dirPath_)) {
            std::filesystem::create_directories(dirPath_);
            log().info << "Created block directory: " << dirPath_;
        }
    } catch (const std::exception& e) {
        log().error << "Failed to create directory " << dirPath_ << ": " << e.what();
        return false;
    }
    
    // Load existing index if it exists
    if (std::filesystem::exists(indexFilePath_)) {
        if (!loadIndex()) {
            log().error << "Failed to load index file";
            return false;
        }
        log().info << "Loaded index with " << blockIndex_.size() << " blocks";
        
        // Find the highest file ID from the index
        for (const auto& [blockId, location] : blockIndex_) {
            if (location.fileId > currentFileId_) {
                currentFileId_ = location.fileId;
            }
        }
    } else {
        log().info << "No existing index file, starting fresh";
    }
    
    // Open existing block files referenced in the index
    std::unordered_map<uint32_t, bool> filesNeeded;
    for (const auto& [blockId, location] : blockIndex_) {
        filesNeeded[location.fileId] = true;
    }
    
    for (const auto& [fileId, _] : filesNeeded) {
        std::string filepath = getBlockFilePath(fileId);
        if (std::filesystem::exists(filepath)) {
            auto ukpBlockFile = std::make_unique<BlockFile>();
            BlockFile::Config config(filepath, maxFileSize_);
            if (ukpBlockFile->init(config)) {
                ukpBlockFiles_[fileId] = std::move(ukpBlockFile);
                log().debug << "Opened existing block file: " << filepath;
            } else {
                log().error << "Failed to open block file: " << filepath;
            }
        }
    }
    
    log().info << "BlockDir initialized with " << ukpBlockFiles_.size() 
               << " files and " << blockIndex_.size() << " blocks";
    
    return true;
}

bool BlockDir::writeBlock(uint64_t blockId, const void* data, size_t size) {
    // Check if block already exists
    if (hasBlock(blockId)) {
        log().warning << "Block " << blockId << " already exists, overwriting not supported";
        return false;
    }
    
    // Get active block file for writing
    BlockFile* blockFile = getActiveBlockFile(size);
    if (!blockFile) {
        log().error << "Failed to get active block file";
        return false;
    }
    
    // Write data to the file
    int64_t offset = blockFile->write(data, size);
    if (offset < 0) {
        log().error << "Failed to write block " << blockId << " to file";
        return false;
    }
    
    // Update index
    blockIndex_[blockId] = BlockLocation(currentFileId_, offset, size);
    
    log().debug << "Wrote block " << blockId << " to file " << currentFileId_ 
                << " at offset " << offset << " (size: " << size << " bytes)";
    
    // Save index after each write (for durability)
    saveIndex();
    
    return true;
}

int64_t BlockDir::readBlock(uint64_t blockId, void* data, size_t maxSize) {
    // Get block location from index
    BlockLocation location;
    if (!getBlockLocation(blockId, location)) {
        log().error << "Block " << blockId << " not found in index";
        return -1;
    }
    
    // Check buffer size
    if (maxSize < location.size) {
        log().error << "Buffer too small for block " << blockId 
                    << " (need " << location.size << ", have " << maxSize << ")";
        return -1;
    }
    
    // Get the block file
    BlockFile* blockFile = getBlockFile(location.fileId);
    if (!blockFile) {
        log().error << "Block file " << location.fileId << " not found";
        return -1;
    }
    
    // Read data from the file
    int64_t bytesRead = blockFile->read(location.offset, data, location.size);
    if (bytesRead != static_cast<int64_t>(location.size)) {
        log().error << "Failed to read block " << blockId << " (read " 
                    << bytesRead << " bytes, expected " << location.size << ")";
        return -1;
    }
    
    log().debug << "Read block " << blockId << " from file " << location.fileId 
                << " at offset " << location.offset << " (size: " << location.size << " bytes)";
    
    return bytesRead;
}

bool BlockDir::getBlockLocation(uint64_t blockId, BlockLocation& location) const {
    auto it = blockIndex_.find(blockId);
    if (it == blockIndex_.end()) {
        return false;
    }
    location = it->second;
    return true;
}

bool BlockDir::hasBlock(uint64_t blockId) const {
    return blockIndex_.find(blockId) != blockIndex_.end();
}

void BlockDir::flush() {
    // Flush all block files
    for (auto& [fileId, ukpBlockFile] : ukpBlockFiles_) {
        ukpBlockFile->flush();
    }
    
    // Save index
    if (!saveIndex()) {
        log().error << "Failed to save index during flush";
    }
}

BlockFile* BlockDir::createBlockFile(uint32_t fileId) {
    std::string filepath = getBlockFilePath(fileId);
    auto ukpBlockFile = std::make_unique<BlockFile>();
    
    BlockFile::Config config(filepath, maxFileSize_);
    if (!ukpBlockFile->init(config)) {
        log().error << "Failed to create block file: " << filepath;
        return nullptr;
    }
    
    log().info << "Created new block file: " << filepath;
    
    BlockFile* pBlockFile = ukpBlockFile.get();
    ukpBlockFiles_[fileId] = std::move(ukpBlockFile);
    return pBlockFile;
}

BlockFile* BlockDir::getActiveBlockFile(size_t dataSize) {
    // Check if current file exists and can fit the data
    auto it = ukpBlockFiles_.find(currentFileId_);
    if (it != ukpBlockFiles_.end() && it->second->canFit(dataSize)) {
        return it->second.get();
    }
    
    // Need to create a new file
    currentFileId_++;
    return createBlockFile(currentFileId_);
}

BlockFile* BlockDir::getBlockFile(uint32_t fileId) {
    auto it = ukpBlockFiles_.find(fileId);
    if (it != ukpBlockFiles_.end()) {
        return it->second.get();
    }
    
    // Try to open the file if it exists
    std::string filepath = getBlockFilePath(fileId);
    if (std::filesystem::exists(filepath)) {
        auto ukpBlockFile = std::make_unique<BlockFile>();
        BlockFile::Config config(filepath, maxFileSize_);
        if (ukpBlockFile->init(config)) {
            BlockFile* pBlockFile = ukpBlockFile.get();
            ukpBlockFiles_[fileId] = std::move(ukpBlockFile);
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
    
    blockIndex_.clear();
    
    // Read index entries
    // Format: [blockId (8 bytes)][fileId (4 bytes)][offset (8 bytes)][size (8 bytes)]
    while (indexFile.good() && !indexFile.eof()) {
        uint64_t blockId;
        uint32_t fileId;
        int64_t offset;
        uint64_t size;
        
        indexFile.read(reinterpret_cast<char*>(&blockId), sizeof(blockId));
        if (indexFile.gcount() == 0) break; // End of file
        
        indexFile.read(reinterpret_cast<char*>(&fileId), sizeof(fileId));
        indexFile.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        indexFile.read(reinterpret_cast<char*>(&size), sizeof(size));
        
        if (indexFile.good()) {
            blockIndex_[blockId] = BlockLocation(fileId, offset, static_cast<size_t>(size));
        }
    }
    
    indexFile.close();
    log().debug << "Loaded " << blockIndex_.size() << " entries from index";
    
    return true;
}

bool BlockDir::saveIndex() {
    std::ofstream indexFile(indexFilePath_, std::ios::binary | std::ios::trunc);
    if (!indexFile.is_open()) {
        log().error << "Failed to open index file for writing: " << indexFilePath_;
        return false;
    }
    
    // Write index entries
    // Format: [blockId (8 bytes)][fileId (4 bytes)][offset (8 bytes)][size (8 bytes)]
    for (const auto& [blockId, location] : blockIndex_) {
        uint64_t id = blockId;
        uint32_t fileId = location.fileId;
        int64_t offset = location.offset;
        uint64_t size = static_cast<uint64_t>(location.size);
        
        indexFile.write(reinterpret_cast<const char*>(&id), sizeof(id));
        indexFile.write(reinterpret_cast<const char*>(&fileId), sizeof(fileId));
        indexFile.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
        indexFile.write(reinterpret_cast<const char*>(&size), sizeof(size));
    }
    
    indexFile.close();
    log().debug << "Saved " << blockIndex_.size() << " entries to index";
    
    return true;
}

} // namespace pp
