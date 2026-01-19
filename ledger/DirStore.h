#ifndef PP_LEDGER_DIR_STORE_H
#define PP_LEDGER_DIR_STORE_H

#include "Module.h"
#include "ResultOrError.hpp"
#include <cstdint>
#include <string>

namespace pp {

/**
 * DirStore is an abstract base class for directory-based stores.
 * It provides common magic numbers and utility functions for FileDirStore and DirDirStore.
 */
class DirStore : public Module {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T> using Roe = ResultOrError<T, Error>;

    /**
     * Magic number constants for index files
     */
    static constexpr uint32_t MAGIC_FILE_DIR = 0x504C4944; // "PLID" (PP Ledger Index Directory)
    static constexpr uint32_t MAGIC_DIR_DIR = 0x504C4444;  // "PLDD" (PP Ledger Dir-Dir)

    DirStore(const std::string &name) : Module(name) {}
    virtual ~DirStore() = default;

    /**
     * Check if the store can accommodate more data
     * @param size Size of data to be written
     * @return true if data can fit, false otherwise
     */
    virtual bool canFit(uint64_t size) const = 0;

    /**
     * Get the number of blocks stored
     * @return Number of blocks
     */
    virtual uint64_t getBlockCount() const = 0;

    /**
     * Read a block by index
     * @param index Block index (0-based)
     * @return Block data as string, or error
     */
    virtual Roe<std::string> readBlock(uint64_t index) const = 0;

    /**
     * Append a block to the store
     * @param block Block data to append
     * @return Block index, or error
     */
    virtual Roe<uint64_t> appendBlock(const std::string &block) = 0;

    /**
     * Rewind to a specific block index (truncate)
     * @param index Block index to rewind to
     * @return Success or error
     */
    virtual Roe<void> rewindTo(uint64_t index) = 0;

    /**
     * Relocates all contents of this store to a subdirectory.
     * After this call, the current directory will only contain the new subdirectory.
     * Uses filesystem rename: renames dir to dir_tmp, creates dir, renames dir_tmp to dir/subdirName
     * @param subdirName The name of the subdirectory to create (e.g., "000001")
     * @return The full path to the new subdirectory on success, or an error
     */
    virtual Roe<std::string> relocateToSubdir(const std::string &subdirName) = 0;

protected:
    /**
     * Format an ID as a zero-padded 6-digit string (e.g., 1 -> "000001")
     * @param id The ID to format
     * @return Formatted string
     */
    static std::string formatId(uint32_t id);

    /**
     * Get the index file path for a directory
     * @param dirPath The directory path
     * @return The index file path (dirPath + "/idx.dat")
     */
    static std::string getIndexFilePath(const std::string &dirPath);

    /**
     * Ensure a directory exists, creating it if necessary
     * @param dirPath The directory path to ensure exists
     * @return Success or error
     */
    Roe<void> ensureDirectory(const std::string &dirPath);

    /**
     * Validate that maxFileSize meets the minimum requirement (1MB)
     * @param maxFileSize The maximum file size to validate
     * @return Success or error if size is too small
     */
    static Roe<void> validateMinFileSize(size_t maxFileSize);

    /**
     * Perform the filesystem operations to relocate directory contents to a subdirectory.
     * Steps: rename dir to temp -> create original dir -> rename temp to subdir
     * @param originalPath The original directory path
     * @param subdirName The name of the subdirectory to create
     * @return The full path to the new subdirectory on success, or an error
     */
    Roe<std::string> performDirectoryRelocation(const std::string &originalPath, 
                                                 const std::string &subdirName);
};

} // namespace pp

#endif // PP_LEDGER_DIR_STORE_H
