#pragma once

#include "Module.h"
#include <string>
#include <fstream>
#include <cstdint>

namespace pp {

/**
 * BlockFile manages writing block data to a single file with a size limit.
 * When the file reaches the configured size limit, it should be closed and
 * a new file should be created by BlockDir.
 */
class BlockFile : public Module {
public:
    /**
     * Configuration for BlockFile initialization
     */
    struct Config {
        std::string filepath;
        size_t maxSize = 100 * 1024 * 1024; // 100MB default
        
        Config() = default;
        Config(const std::string& path, size_t size = 100 * 1024 * 1024)
            : filepath(path), maxSize(size) {}
    };
    
    /**
     * Constructor
     */
    BlockFile();
    
    /**
     * Destructor - ensures file is properly closed
     */
    ~BlockFile();
    
    /**
     * Initialize the block file
     * @param config Configuration for the block file
     * @return true on success, false on error
     */
    bool init(const Config& config);
    
    // Delete copy constructor and assignment
    BlockFile(const BlockFile&) = delete;
    BlockFile& operator=(const BlockFile&) = delete;
    
    /**
     * Write block data to the file
     * @param data Block data to write
     * @param size Size of the data in bytes
     * @return Offset where data was written, or -1 on error
     */
    int64_t write(const void* data, size_t size);
    
    /**
     * Read block data from the file
     * @param offset Offset in the file to read from
     * @param data Buffer to read data into
     * @param size Number of bytes to read
     * @return Number of bytes read, or -1 on error
     */
    int64_t read(int64_t offset, void* data, size_t size);
    
    /**
     * Check if the file can accommodate more data
     * @param size Size of data to be written
     * @return true if data can fit, false otherwise
     */
    bool canFit(size_t size) const;
    
    /**
     * Get current file size
     */
    size_t getCurrentSize() const { return currentSize_; }
    
    /**
     * Get maximum file size
     */
    size_t getMaxSize() const { return maxSize_; }
    
    /**
     * Get file path
     */
    const std::string& getFilePath() const { return filepath_; }
    
    /**
     * Check if file is open and ready for operations
     */
    bool isOpen() const;
    
    /**
     * Close the file
     */
    void close();
    
    /**
     * Flush any buffered data to disk
     */
    void flush();

private:
    std::string filepath_;
    size_t maxSize_;
    size_t currentSize_;
    std::fstream file_;
    
    /**
     * Open the file for reading and writing
     * @return true on success, false on error
     */
    bool open();
};

} // namespace pp
