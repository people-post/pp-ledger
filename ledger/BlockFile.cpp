#include "BlockFile.h"
#include "Logger.h"
#include <filesystem>

namespace pp {

BlockFile::BlockFile()
    : Module("blockfile")
    , maxSize_(0)
    , currentSize_(0) {
}

BlockFile::Roe<void> BlockFile::init(const Config& config) {
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
    
    auto result = open();
    if (!result) {
        log().error << "Failed to open file: " << filepath_;
        return result.error();
    }
    
    return Roe<void>();
}

BlockFile::~BlockFile() {
    close();
}

BlockFile::Roe<void> BlockFile::open() {
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
    
    if (!file_.is_open()) {
        return Error("Failed to open file: " + filepath_);
    }
    
    return Roe<void>();
}

BlockFile::Roe<int64_t> BlockFile::write(const void* data, size_t size) {
    if (!isOpen()) {
        log().error << "File is not open: " << filepath_;
        return Error("File is not open: " + filepath_);
    }
    
    if (!canFit(size)) {
        log().warning << "Cannot fit " << size << " bytes (current: " << currentSize_ 
                      << ", max: " << maxSize_ << ")";
        return Error("Cannot fit " + std::to_string(size) + " bytes");
    }
    
    // Get current position (end of file)
    file_.seekp(0, std::ios::end);
    int64_t offset = file_.tellp();
    
    // Write data
    file_.write(static_cast<const char*>(data), size);
    
    if (!file_.good()) {
        log().error << "Failed to write data to file: " << filepath_;
        return Error("Failed to write data to file: " + filepath_);
    }
    
    currentSize_ += size;
    log().debug << "Wrote " << size << " bytes at offset " << offset 
                << " (total size: " << currentSize_ << ")";
    
    return Roe<int64_t>(offset);
}

BlockFile::Roe<int64_t> BlockFile::read(int64_t offset, void* data, size_t size) {
    if (!isOpen()) {
        log().error << "File is not open: " << filepath_;
        return Error("File is not open: " + filepath_);
    }
    
    // Seek to the offset
    file_.seekg(offset, std::ios::beg);
    
    if (!file_.good()) {
        log().error << "Failed to seek to offset " << offset << " in file: " << filepath_;
        return Error("Failed to seek to offset " + std::to_string(offset));
    }
    
    // Read data
    file_.read(static_cast<char*>(data), size);
    
    int64_t bytesRead = file_.gcount();
    
    if (bytesRead != static_cast<int64_t>(size)) {
        log().warning << "Read " << bytesRead << " bytes, expected " << size;
    }
    
    return Roe<int64_t>(bytesRead);
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
