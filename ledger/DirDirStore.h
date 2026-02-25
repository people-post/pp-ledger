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
 * 
 * Recursion Strategy (Breadth-First):
 * - When maxDirCount is reached at a level, creates DirDirStore children at that level
 * - Those DirDirStore children initially only create FileDirStore children (not deeper recursion)
 * - Only when all direct children at a level are DirDirStore (no FileDirStore left) does it
 *   allow those DirDirStore children to create deeper recursive DirDirStore children
 * - This ensures breadth-first expansion: all direct children are created before going deeper
 */
class DirDirStore : public DirStore {
public:
    struct InitConfig {
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
    };

    struct MountConfig {
        std::string dirPath;
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
         * When mount() is called on an existing directory:
         * - maxDirCount, maxFileCount, and maxFileSize are read from the index file
         *   (saved during init())
         * - Only maxLevel can be changed between runs to control recursion depth
         * - The saved config values are used for:
         *   * Opening existing subdirectory stores (FileDirStore/DirDirStore)
         *   * Creating new subdirectory stores
         *   * Creating new recursive DirDirStore children
         * 
         * Important considerations:
         * - Changing maxLevel: Affects only new recursive DirDirStore creation.
         *   Existing recursive stores retain their current level but will use the new
         *   maxLevel for future recursive store creation.
         * 
         * - dirPath: The directory path must match the existing directory structure.
         */
    };

    DirDirStore();
    ~DirDirStore() override;

    bool canFit(uint64_t size) const override;

    uint64_t getBlockCount() const override;

    /**
     * Initialize a new DirDirStore.
     * Creates a new directory structure and index file.
     * Fails if the index file already exists.
     * @param config Configuration for the new store
     * @return Success or error
     */
    Roe<void> init(const InitConfig &config);

    /**
     * Mount an existing DirDirStore.
     * Loads the existing directory structure and index file.
     * Fails if the index file or directory does not exist.
     * @param config Configuration for mounting the existing store
     * @return Success or error
     */
    Roe<void> mount(const MountConfig &config);

    /**
     * Get the current nesting level of this DirDirStore
     * @return Current level (0 for root, 1 for first child, etc.)
     */
    size_t getCurrentLevel() const { return currentLevel_; }

    Roe<std::string> readBlock(uint64_t index) const override;
    Roe<uint64_t> appendBlock(const std::string &block) override;
    Roe<void> rewindTo(uint64_t index) override;
    uint64_t countSizeFromBlockId(uint64_t blockId) const override;

    /**
     * Relocates all contents of this store to a subdirectory.
     * This is used during transition when the store needs to be nested
     * under a parent DirDirStore.
     * @param subdirName The name of the subdirectory (e.g., "000001")
     * @param excludeFiles Files to exclude from relocation (kept in parent)
     * @return The full path to the new subdirectory on success
     */
    Roe<std::string> relocateToSubdir(const std::string &subdirName,
                                       const std::vector<std::string> &excludeFiles = {}) override;

private:
    static constexpr const char* DIRDIR_INDEX_FILENAME = "dirdir_idx.dat";

    struct Config {
        std::string dirPath;
        size_t maxDirCount{ 0 };
        size_t maxFileCount{ 0 };
        size_t maxFileSize{ 0 };
        size_t maxLevel{ 0 };
    };

    /**
     * Internal init method that accepts a starting level
     * Used when creating child DirDirStores with proper level tracking
     * @param config The configuration
     * @param level The starting level for this store
     * @return Success or error
     */
    Roe<void> initWithLevel(const InitConfig &config, size_t level);

    /**
     * Internal mount method that accepts a starting level
     * Used when mounting existing child DirDirStores with proper level tracking
     * @param config The configuration
     * @param level The starting level for this store
     * @return Success or error
     */
    Roe<void> mountWithLevel(const MountConfig &config, size_t level);

    /**
     * Get the DirDirStore index file path for a directory
     * @param dirPath The directory path
     * @return The index file path (dirPath + "/" + DIRDIR_INDEX_FILENAME)
     */
    static std::string getDirDirIndexFilePath(const std::string &dirPath);

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
        uint64_t maxDirCount{ 0 };
        uint64_t maxFileCount{ 0 };
        uint64_t maxFileSize{ 0 };

        IndexFileHeader() = default;

        template <typename Archive> void serialize(Archive &ar) {
            ar &magic &version &reserved &headerSize &dirCount &maxDirCount &maxFileCount &maxFileSize;
        }
    };

    /**
     * Structure representing a directory's starting block index
     */
    struct DirIndexEntry {
        uint32_t dirId{ 0 };
        uint64_t startBlockId{ 0 };
        bool isRecursive{ false }; // true if it's a DirDirStore, false if FileDirStore

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
    Roe<void> initRootStoreMode(bool isMount);
    Roe<void> openExistingSubdirectoryStores();
    Roe<void> reopenSubdirectoryStores();
    void recalculateTotalBlockCount();
    void updateCurrentDirId();
    Roe<void> openDirStore(DirInfo &dirInfo, uint32_t dirId, const std::string &dirpath);
};

} // namespace pp

#endif // PP_LEDGER_DIR_DIR_STORE_H
