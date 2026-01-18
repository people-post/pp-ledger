#ifndef PP_LEDGER_DIR_STORE_H
#define PP_LEDGER_DIR_STORE_H

#include "BlockStore.hpp"
#include <cstdint>
#include <string>

namespace pp {

/**
 * DirStore is an abstract base class for directory-based BlockStores.
 * It provides common magic numbers for FileDirStore and DirDirStore.
 */
class DirStore : public BlockStore {
public:
    /**
     * Magic number constants for index files
     */
    static constexpr uint32_t MAGIC_FILE_DIR = 0x504C4944; // "PLID" (PP Ledger Index Directory)
    static constexpr uint32_t MAGIC_DIR_DIR = 0x504C4444;  // "PLDD" (PP Ledger Dir-Dir)

    DirStore(const std::string &name) : BlockStore(name) {}
    virtual ~DirStore() = default;

    /**
     * Relocates all contents of this store to a subdirectory.
     * After this call, the current directory will only contain the new subdirectory.
     * Uses filesystem rename: renames dir to dir_tmp, creates dir, renames dir_tmp to dir/subdirName
     * @param subdirName The name of the subdirectory to create (e.g., "000001")
     * @return The full path to the new subdirectory on success, or an error
     */
    virtual Roe<std::string> relocateToSubdir(const std::string &subdirName) = 0;
};

} // namespace pp

#endif // PP_LEDGER_DIR_STORE_H
