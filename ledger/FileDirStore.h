#ifndef PP_LEDGER_FILE_DIR_STORE_H
#define PP_LEDGER_FILE_DIR_STORE_H

#include "DirStore.h"
#include "FileStore.h"
#include "../lib/BinaryPack.hpp"
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

/**
 * FileDirStore stores blocks in a directory of files.
 * It implements the DirStore interface for file-based storage.
 */
class FileDirStore : public DirStore {
public:
    struct InitConfig {
        std::string dirPath;
        size_t maxFileCount{ 0 };
        size_t maxFileSize{ 0 };

        /**
         * Config behavior:
         * 
         * For init():
         * - Creates a new directory with the specified config values
         * - Directory must NOT already exist
         * 
         * For mount():
         * - Config values are NOT persisted or read from the existing directory
         * - The config values provided to mount() are stored and used for:
         *   * Opening existing block files (FileStore instances)
         *   * Creating new block files
         * 
         * This means:
         * - Config values can be changed between runs
         * - New config values will be applied when reopening existing files
         * - Existing files are reopened using the new config values, not the original ones
         * 
         * Important considerations when mounting with different config:
         * - Changing maxFileSize: Existing FileStore instances will be reopened with the
         *   new maxFileSize. This should generally be safe if only increasing the value.
         *   Decreasing may cause issues if existing files exceed the new limit.
         * 
         * - Changing maxFileCount: Affects only new file creation. Existing files are
         *   retained, but the new limit will be enforced for future file creation.
         */
    };

    FileDirStore();
    virtual ~FileDirStore();

    bool canFit(uint64_t size) const override;

    uint64_t getBlockCount() const override;

    /**
     * Initialize a new FileDirStore (creates new directory)
     * @param config Configuration for the new store
     * @return Error if directory already exists or creation fails
     */
    Roe<void> init(const InitConfig &config);

    /**
     * Mount an existing FileDirStore (loads existing directory)
     * @param dirPath Path to existing directory
     * @param maxFileCount Maximum number of files
     * @param maxFileSize Maximum size per file
     * @return Error if directory doesn't exist or loading fails
     */
    Roe<void> mount(const std::string &dirPath, size_t maxFileCount, size_t maxFileSize);

    Roe<std::string> readBlock(uint64_t index) const override;
    Roe<uint64_t> appendBlock(const std::string &block) override;
    Roe<void> rewindTo(uint64_t index) override;

    /**
     * Relocates all contents of this store to a subdirectory.
     * This is used during transition when the store needs to be nested
     * under a parent DirDirStore.
     * @param subdirName The name of the subdirectory (e.g., "000001")
     * @return The full path to the new subdirectory on success
     */
    Roe<std::string> relocateToSubdir(const std::string &subdirName) override;

private:
    struct Config {
        std::string dirPath;
        size_t maxFileCount{ 0 };
        size_t maxFileSize{ 0 };
    };

    /**
     * Index file header structure
     */
    struct IndexFileHeader {
        static constexpr uint32_t MAGIC = MAGIC_FILE_DIR;
        static constexpr uint16_t CURRENT_VERSION = 1;

        uint32_t magic{ MAGIC };
        uint16_t version{ CURRENT_VERSION };
        uint16_t reserved{ 0 };
        uint64_t headerSize{ sizeof(IndexFileHeader) };

        IndexFileHeader() = default;

        template <typename Archive> void serialize(Archive &ar) {
            ar &magic &version &reserved &headerSize;
        }
    };

    /**
     * Structure representing a file's starting block index
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
     * Structure holding FileStore and its starting block index
     */
    struct FileInfo {
        std::unique_ptr<FileStore> blockFile;
        uint64_t startBlockId;
    };

    InitConfig config_;
    uint32_t currentFileId_{ 0 };
    std::string indexFilePath_;

    // Block files with their starting block indices, indexed by file ID
    std::unordered_map<uint32_t, FileInfo> fileInfoMap_;

    // Ordered list of file IDs (tracks creation/addition order)
    std::vector<uint32_t> fileIdOrder_;

    // Total block count across all files
    uint64_t totalBlockCount_{ 0 };

    FileStore *createBlockFile(uint32_t fileId, uint64_t startBlockId);
    FileStore *getActiveBlockFile(uint64_t dataSize);
    FileStore *getBlockFile(uint32_t fileId);
    std::string getBlockFilePath(uint32_t fileId) const;
    std::pair<uint32_t, uint64_t> findBlockFile(uint64_t blockId) const;

    // Index operations
    bool loadIndex();
    bool saveIndex();
    bool writeIndexHeader(std::ostream &os);
    bool readIndexHeader(std::istream &is);
    void flush();

    // Helper methods for init and relocate
    Roe<void> openExistingBlockFiles();
    Roe<void> reopenBlockFiles();
    void recalculateTotalBlockCount();
    void updateCurrentFileId();
};

} // namespace pp

#endif // PP_LEDGER_FILE_DIR_STORE_H