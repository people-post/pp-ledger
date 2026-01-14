#include "BlockFile.h"
#include "Logger.h"
#include <filesystem>

namespace pp {

BlockFile::BlockFile()
    : Module("blockfile")
    , maxSize_(0)
    , currentSize_(0)
    , headerValid_(false) {
}

BlockFile::Roe<void> BlockFile::init(const Config& config) {
    filepath_ = config.filepath;
    maxSize_ = config.maxSize;
    currentSize_ = 0;
    headerValid_ = false;
    
    bool fileExists = std::filesystem::exists(filepath_);
    
    auto result = open();
    if (!result) {
        log().error << "Failed to open file: " << filepath_;
        return result.error();
    }
    
    if (fileExists) {
        // Read and validate existing header
        auto headerResult = readHeader();
        if (!headerResult) {
            log().error << "Failed to read header from existing file: " << filepath_;
            return headerResult.error();
        }
        
        // Get total file size (including header)
        currentSize_ = std::filesystem::file_size(filepath_);
        
        log().debug << "Opening existing file: " << filepath_ 
                    << " (total size: " << currentSize_ << " bytes, version: " 
                    << header_.version << ")";
    } else {
        // Write header for new file
        auto headerResult = writeHeader();
        if (!headerResult) {
            log().error << "Failed to write header to new file: " << filepath_;
            return headerResult.error();
        }
        currentSize_ = HEADER_SIZE;
        log().debug << "Created new file with header: " << filepath_;
    }
    
    return {};
}

BlockFile::~BlockFile() {
    close();
}

BlockFile::Roe<void> BlockFile::open() {
    // Open file in binary mode for both reading and writing
    // For existing files, we'll use in|out mode (not app) so we can read header
    // For new files, we'll create them first
    bool fileExists = std::filesystem::exists(filepath_);
    
    if (fileExists) {
        // Open existing file for reading and writing
        file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out);
    } else {
        // Create new file
        file_.open(filepath_, std::ios::binary | std::ios::out);
        if (file_.is_open()) {
            file_.close();
            // Reopen for reading and writing
            file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out);
        }
    }
    
    if (!file_.is_open()) {
        return Error("Failed to open file: " + filepath_);
    }
    
    return {};
}

BlockFile::Roe<int64_t> BlockFile::write(const void* data, size_t size) {
    if (!isOpen()) {
        log().error << "File is not open: " << filepath_;
        return Error("File is not open: " + filepath_);
    }
    
    if (!hasValidHeader()) {
        log().error << "File header is not valid: " << filepath_;
        return Error("File header is not valid: " + filepath_);
    }
    
    if (!canFit(size)) {
        log().warning << "Cannot fit " << size << " bytes (current: " << currentSize_ 
                      << ", max: " << maxSize_ << ")";
        return Error("Cannot fit " + std::to_string(size) + " bytes");
    }
    
    // Seek to end of file
    file_.seekp(0, std::ios::end);
    int64_t fileOffset = file_.tellp();
    
    // Write data
    file_.write(static_cast<const char*>(data), size);
    
    if (!file_.good()) {
        log().error << "Failed to write data to file: " << filepath_;
        return Error("Failed to write data to file: " + filepath_);
    }
    
    // Flush data to disk
    file_.flush();
    if (!file_.good()) {
        log().error << "Failed to flush data to file: " << filepath_;
        return Error("Failed to flush data to file: " + filepath_);
    }
    
    currentSize_ += size;
    log().debug << "Wrote " << size << " bytes at file offset " << fileOffset 
                << " (total file size: " << currentSize_ << ")";
    
    // Return file offset (from file start, including header)
    return fileOffset;
}

BlockFile::Roe<int64_t> BlockFile::read(int64_t offset, void* data, size_t size) {
    if (!isOpen()) {
        log().error << "File is not open: " << filepath_;
        return Error("File is not open: " + filepath_);
    }
    
    if (!hasValidHeader()) {
        log().error << "File header is not valid: " << filepath_;
        return Error("File header is not valid: " + filepath_);
    }
    
    // Offset is from file start (including header)
    // Seek to the file offset
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
    
    return bytesRead;
}

bool BlockFile::canFit(size_t size) const {
    // currentSize_ already includes header, so just check total size
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

BlockFile::Roe<void> BlockFile::writeHeader() {
    if (!isOpen()) {
        return Error("File is not open: " + filepath_);
    }
    
    // Seek to beginning of file
    file_.seekp(0, std::ios::beg);
    
    // Initialize header
    header_ = FileHeader();
    
    // Write header
    file_.write(reinterpret_cast<const char*>(&header_), sizeof(FileHeader));
    
    if (!file_.good()) {
        return Error("Failed to write header to file: " + filepath_);
    }
    
    // Flush header to disk
    file_.flush();
    if (!file_.good()) {
        return Error("Failed to flush header to file: " + filepath_);
    }
    
    headerValid_ = true;
    log().debug << "Wrote file header (magic: 0x" << std::hex << header_.magic 
                << std::dec << ", version: " << header_.version << ")";
    
    return {};
}

BlockFile::Roe<void> BlockFile::readHeader() {
    if (!isOpen()) {
        return Error("File is not open: " + filepath_);
    }
    
    // Seek to beginning of file
    file_.seekg(0, std::ios::beg);
    
    // Read header
    file_.read(reinterpret_cast<char*>(&header_), sizeof(FileHeader));
    
    if (file_.gcount() != static_cast<std::streamsize>(sizeof(FileHeader))) {
        return Error("Failed to read complete header from file: " + filepath_);
    }
    
    // Validate header
    if (header_.magic != FileHeader::MAGIC) {
        return Error("Invalid magic number in file header: " + filepath_);
    }
    
    if (header_.version > FileHeader::CURRENT_VERSION) {
        return Error("Unsupported file version " + std::to_string(header_.version) 
                     + " (current: " + std::to_string(FileHeader::CURRENT_VERSION) + ")");
    }
    
    headerValid_ = true;
    log().debug << "Read file header (magic: 0x" << std::hex << header_.magic 
                << std::dec << ", version: " << header_.version << ")";
    
    return {};
}

bool BlockFile::hasValidHeader() const {
    return headerValid_ && header_.magic == FileHeader::MAGIC;
}

} // namespace pp
