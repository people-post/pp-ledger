#ifndef PP_LEDGER_DIR_STORE_H
#define PP_LEDGER_DIR_STORE_H

#include "BlockStore.hpp"
#include "FileStore.h"
#include <cstdint>
#include <memory>
#include <string>

namespace pp {

/**
 * DirStore is an abstract base class for directory-based BlockStores.
 * It provides common magic numbers and shared structures for FileDirStore and DirDirStore.
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

protected:
    /**
     * Structure representing a file's starting block index.
     * Common to both FileDirStore and DirDirStore.
     */
    struct FileIndexEntry {
        uint32_t fileId;
        uint64_t startBlockId;

        FileIndexEntry() : fileId(0), startBlockId(0) {}
        FileIndexEntry(uint32_t fid, uint64_t startId) 
            : fileId(fid), startBlockId(startId) {}

        template <typename Archive> void serialize(Archive &ar) {
            ar &fileId &startBlockId;
        }
    };

    /**
     * Structure holding FileStore and its starting block index.
     * Common to both FileDirStore and DirDirStore.
     */
    struct FileInfo {
        std::unique_ptr<FileStore> blockFile;
        uint64_t startBlockId;
    };
};

} // namespace pp

#endif // PP_LEDGER_DIR_STORE_H
