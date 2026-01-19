#ifndef PP_LEDGER_DIR_DIR_STORE_H
#define PP_LEDGER_DIR_DIR_STORE_H

#include "DirStore.h"
#include "FileDirStore.h"
#include "../lib/BinaryPack.hpp"
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

/**
 * DirDirStore stores blocks in a directory of directories.
 * It implements the DirStore interface for directory-based storage.
 * 
 * Behavior:
 * 1. Initially uses a FileDirStore at the root level to manage files
 * 2. When the root FileDirStore is full, relocates it to a subdirectory (e.g., "000001")
 * 3. Creates new FileDirStores in subdirectories as needed
 * 4. When maxDirCount is reached, creates deeper subdirectories with DirDirStores (recursive)
 */
class DirDirStore : public DirStore {
public:
    struct Config {
        std::string dirPath;
        size_t maxDirCount{ 0 };
        size_t maxFileCount{ 0 };
        size_t maxFileSize{ 0 };
        /**
         * Maximum nesting level for recursive DirDirStores.
         * Level 0 means only FileDirStores as children (no recursive DirDirStore).
         * Level 1 means DirDirStore children can have FileDirStore children only.
         * Level N means up to N levels of nested DirDirStores.
         * Default is 0 (no recursion).
         */
        size_t maxLevel{ 0 };

        /**
         * Behavior when operating on existing directories:
         * 
         * When init() is called on an existing directory:
         * - Config values are NOT persisted or read from the existing directory
         * - The config values provided to init() are stored and used for:
         *   * Opening existing subdirectory stores (FileDirStore/DirDirStore)
         *   * Creating new subdirectory stores
         *   * Creating new recursive DirDirStore children
         * 
         * This means:
         * - Config values can be changed between runs
         * - New config values will be applied when reopening existing stores
         * - Existing stores are reopened using the new config values, not the original ones
         * 
         * Important considerations:
         * - Changing maxFileSize: Existing FileStore instances will be reopened with the
         *   new maxFileSize. This should generally be safe if only increasing the value.
         *   Decreasing may cause issues if existing files exceed the new limit.
         * 
         * - Changing maxFileCount: Affects only new FileDirStore instances. Existing
         *   FileDirStore instances retain their current file count but will use the
         *   new limit for future file creation.
         * 
         * - Changing maxDirCount: Affects only new DirDirStore instances. Existing
         *   DirDirStore instances retain their current directory count but will use
         *   the new limit for future directory creation.
         * 
         * - Changing maxLevel: Affects only new recursive DirDirStore creation.
         *   Existing recursive stores retain their current level but will use the new
         *   maxLevel for future recursive store creation.
         * 
         * - Changing dirPath: The directory path is updated in the config, but this
         *   should typically match the existing directory structure.
         */
    };

    DirDirStore(const std::string &name);
    virtual ~DirDirStore();

    bool canFit(uint64_t size) const override;

    uint64_t getBlockCount() const override;

    Roe<void> init(const Config &config);

    /**
     * Get the current nesting level of this DirDirStore
     * @return Current level (0 for root, 1 for first child, etc.)
     */
    size_t getCurrentLevel() const { return currentLevel_; }

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
    /**
     * Internal init method that accepts a starting level
     * Used when creating child DirDirStores with proper level tracking
     * @param config The configuration
     * @param level The starting level for this store
     * @return Success or error
     */
    Roe<void> initWithLevel(const Config &config, size_t level);

    /**
     * Index file header structure
     */
    struct IndexFileHeader {
        static constexpr uint32_t MAGIC = MAGIC_DIR_DIR;
        static constexpr uint16_t CURRENT_VERSION = 1;

        uint32_t magic{ MAGIC };
        uint16_t version{ CURRENT_VERSION };
        uint16_t reserved{ 0 };
        uint64_t headerSize{ sizeof(IndexFileHeader) };
        uint32_t dirCount{ 0 };    // Number of dir entries (0 means using rootStore_)

        IndexFileHeader() = default;

        template <typename Archive> void serialize(Archive &ar) {
            ar &magic &version &reserved &headerSize &dirCount;
        }
    };

    /**
     * Structure representing a directory's starting block index
     */
    struct DirIndexEntry {
        uint32_t dirId;
        uint64_t startBlockId;
        bool isRecursive; // true if it's a DirDirStore, false if FileDirStore

        DirIndexEntry() : dirId(0), startBlockId(0), isRecursive(false) {}
        DirIndexEntry(uint32_t did, uint64_t startId, bool recursive) 
            : dirId(did), startBlockId(startId), isRecursive(recursive) {}

        template <typename Archive> void serialize(Archive &ar) {
            ar &dirId &startBlockId &isRecursive;
        }
    };

    /**
     * Structure holding FileDirStore or DirDirStore and its starting block index
     */
    struct DirInfo {
        std::unique_ptr<FileDirStore> fileDirStore;
        std::unique_ptr<DirDirStore> dirDirStore;
        uint64_t startBlockId;
        bool isRecursive{ false };
    };

    Config config_;
    uint32_t currentDirId_{ 0 };
    std::string indexFilePath_;
    size_t currentLevel_{ 0 };  // Current nesting level (0 for root)

    // Root FileDirStore - manages files at the root level before any subdirectories are created
    std::unique_ptr<FileDirStore> rootStore_;

    // Directories with their starting block indices, indexed by dir ID
    std::unordered_map<uint32_t, DirInfo> dirInfoMap_;

    // Ordered list of dir IDs (tracks creation/addition order)
    std::vector<uint32_t> dirIdOrder_;

    // Total block count across all stores
    uint64_t totalBlockCount_{ 0 };

    DirStore *getActiveDirStore(uint64_t dataSize);
    FileDirStore *createFileDirStore(uint32_t dirId, uint64_t startBlockId);
    DirDirStore *createDirDirStore(uint32_t dirId, uint64_t startBlockId);
    std::string getDirPath(uint32_t dirId) const;
    std::pair<uint32_t, uint64_t> findBlockDir(uint64_t blockId) const;

    /**
     * Check if this store can create recursive DirDirStore children
     * @return true if recursion is allowed at current level, false otherwise
     */
    bool canCreateRecursive() const;

    Roe<void> relocateRootStore();

    // Index operations
    bool loadIndex();
    bool saveIndex();
    bool writeIndexHeader(std::ostream &os);
    bool readIndexHeader(std::istream &is);
    void flush();

    // Helper methods for init and relocate
    Roe<bool> detectStoreMode();
    Roe<void> initRootStoreMode();
    Roe<void> openExistingSubdirectoryStores();
    Roe<void> reopenSubdirectoryStores();
    void recalculateTotalBlockCount();
    void updateCurrentDirId();
    Roe<void> openDirStore(DirInfo &dirInfo, const std::string &dirpath);
};

} // namespace pp

#endif // PP_LEDGER_DIR_DIR_STORE_H
