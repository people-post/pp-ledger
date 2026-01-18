#ifndef PP_LEDGER_DIR_DIR_STORE_H
#define PP_LEDGER_DIR_DIR_STORE_H

#include "BlockStore.hpp"
#include "FileDirStore.h"
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
 * DirDirStore is a BlockStore that stores blocks in a directory of directories.
 * It implements the BlockStore interface for directory-based storage.
 * 
 * Behavior:
 * 1. Initially manages FileStores directly (like FileDirStore)
 * 2. When maxFileCount is reached, creates subdirectories with FileDirStores
 * 3. When maxDirCount is reached, creates deeper subdirectories with DirDirStores (recursive)
 */
class DirDirStore : public BlockStore {
public:
    struct Config {
        std::string dirPath;
        size_t maxDirCount{ 0 };
        size_t maxFileCount{ 0 };
        size_t maxFileSize{ 0 };
    };

    DirDirStore(const std::string &name);
    virtual ~DirDirStore();

    bool canFit(uint64_t size) const override;

    uint64_t getBlockCount() const override;

    Roe<void> init(const Config &config);

    Roe<std::string> readBlock(uint64_t index) const override;
    Roe<uint64_t> appendBlock(const std::string &block) override;
    Roe<void> rewindTo(uint64_t index) override;

private:
    enum class Mode {
        FILES,  // Managing FileStores directly
        DIRS    // Managing FileDirStores or DirDirStores
    };

    /**
     * Index file header structure
     */
    struct IndexFileHeader {
        static constexpr uint32_t MAGIC = 0x504C4444; // "PLDD" (PP Ledger Dir-Dir)
        static constexpr uint16_t CURRENT_VERSION = 1;

        uint32_t magic{ MAGIC };
        uint16_t version{ CURRENT_VERSION };
        uint16_t reserved{ 0 };
        uint64_t headerSize{ sizeof(IndexFileHeader) };
        uint32_t fileCount{ 0 };  // Number of file entries
        uint32_t dirCount{ 0 };    // Number of dir entries

        IndexFileHeader() = default;

        template <typename Archive> void serialize(Archive &ar) {
            ar &magic &version &reserved &headerSize &fileCount &dirCount;
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
     * Structure holding FileStore and its starting block index
     */
    struct FileInfo {
        std::unique_ptr<FileStore> blockFile;
        uint64_t startBlockId;
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
    Mode mode_{ Mode::FILES };
    uint32_t currentFileId_{ 0 };
    uint32_t currentDirId_{ 0 };
    std::string indexFilePath_;

    // Block files with their starting block indices, indexed by file ID
    std::unordered_map<uint32_t, FileInfo> fileInfoMap_;

    // Ordered list of file IDs (tracks creation/addition order)
    std::vector<uint32_t> fileIdOrder_;

    // Directories with their starting block indices, indexed by dir ID
    std::unordered_map<uint32_t, DirInfo> dirInfoMap_;

    // Ordered list of dir IDs (tracks creation/addition order)
    std::vector<uint32_t> dirIdOrder_;

    // Total block count across all files and dirs
    uint64_t totalBlockCount_{ 0 };

    FileStore *createBlockFile(uint32_t fileId, uint64_t startBlockId);
    FileStore *getActiveBlockFile(uint64_t dataSize);
    FileStore *getBlockFile(uint32_t fileId);
    std::string getBlockFilePath(uint32_t fileId) const;
    std::pair<uint32_t, uint64_t> findBlockFile(uint64_t blockId) const;

    BlockStore *getActiveDirStore(uint64_t dataSize);
    FileDirStore *createFileDirStore(uint32_t dirId, uint64_t startBlockId);
    DirDirStore *createDirDirStore(uint32_t dirId, uint64_t startBlockId);
    std::string getDirPath(uint32_t dirId) const;
    std::pair<uint32_t, uint64_t> findBlockDir(uint64_t blockId) const;

    bool loadIndex();
    bool saveIndex();
    bool writeIndexHeader(std::ostream &os);
    bool readIndexHeader(std::istream &is);
    void flush();
};

} // namespace pp

#endif // PP_LEDGER_DIR_DIR_STORE_H 