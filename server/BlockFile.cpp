#include "BlockFile.h"
#include "Logger.h"
#include <filesystem>

namespace pp {

BlockFile::BlockFile()
    : Module("blockfile")
    , maxSize_(0)
    , currentSize_(0) {
}

bool BlockFile::init(const Config& config) {
    filepath_ = config.filepath;
    maxSize_ = config.maxSize;
    currentSize_ = 0;
    
    // Check if file already exists to get its current size
    if (std::filesystem::exists(filepath_)) {
        currentSize_ = std::filesystem::file_size(filepath_);
        log().debug << "Opening existing file: " << filepath_ << " (size: " << currentSize_ << " bytes)";
    } else {
        log().debug << "Creating new file: " << filepath_;
    }
    
    if (!open()) {
        log().error << "Failed to open file: " << filepath_;
        return false;
    }
    
    return true;
}

BlockFile::~BlockFile() {
    close();
}

bool BlockFile::open() {
    // Open file in binary mode for both reading and writing
    // Create file if it doesn't exist
    file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    
    if (!file_.is_open()) {
        // Try creating the file
        file_.open(filepath_, std::ios::binary | std::ios::out);
        if (file_.is_open()) {
            file_.close();
            file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out);
        }
    }
    
    return file_.is_open();
}

int64_t BlockFile::write(const void* data, size_t size) {
    if (!isOpen()) {
        log().error << "File is not open: " << filepath_;
        return -1;
    }
    
    if (!canFit(size)) {
        log().warning << "Cannot fit " << size << " bytes (current: " << currentSize_ 
                      << ", max: " << maxSize_ << ")";
        return -1;
    }
    
    // Get current position (end of file)
    file_.seekp(0, std::ios::end);
    int64_t offset = file_.tellp();
    
    // Write data
    file_.write(static_cast<const char*>(data), size);
    
    if (!file_.good()) {
        log().error << "Failed to write data to file: " << filepath_;
        return -1;
    }
    
    currentSize_ += size;
    log().debug << "Wrote " << size << " bytes at offset " << offset 
                << " (total size: " << currentSize_ << ")";
    
    return offset;
}

int64_t BlockFile::read(int64_t offset, void* data, size_t size) {
    if (!isOpen()) {
        log().error << "File is not open: " << filepath_;
        return -1;
    }
    
    // Seek to the offset
    file_.seekg(offset, std::ios::beg);
    
    if (!file_.good()) {
        log().error << "Failed to seek to offset " << offset << " in file: " << filepath_;
        return -1;
    }
    
    // Read data
    file_.read(static_cast<char*>(data), size);
    
    int64_t bytesRead = file_.gcount();
    
    if (bytesRead != static_cast<int64_t>(size)) {
        log().warning << "Read " << bytesRead << " bytes, expected " << size;
    }
    
    return bytesRead;
}

bool BlockFile::canFit(size_t size) const {
    return (currentSize_ + size) <= maxSize_;
}

bool BlockFile::isOpen() const {
    return file_.is_open() && file_.good();
}

void BlockFile::close() {
    if (file_.is_open()) {
        file_.close();
        log().debug << "Closed file: " << filepath_;
    }
}

void BlockFile::flush() {
    if (file_.is_open()) {
        file_.flush();
    }
}

} // namespace pp
