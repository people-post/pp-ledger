#ifndef PP_LEDGER_FILE_DIR_STORE_H
#define PP_LEDGER_FILE_DIR_STORE_H

#include "DirStore.h"
#include "../lib/BinaryPack.hpp"
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

/**
 * FileDirStore is a BlockStore that stores blocks in a directory of files.
 * It implements the BlockStore interface for file-based storage.
 */
class FileDirStore : public DirStore {
public:
    struct Config {
        std::string dirPath;
        size_t maxFileCount{ 0 };
        size_t maxFileSize{ 0 };
    };

    FileDirStore(const std::string &name);
    virtual ~FileDirStore();

    bool canFit(uint64_t size) const override;

    uint64_t getBlockCount() const override;

    Roe<void> init(const Config &config);

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

    Config config_;
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
    bool loadIndex();
    bool saveIndex();
    bool writeIndexHeader(std::ostream &os);
    bool readIndexHeader(std::istream &is);
    void flush();
};

} // namespace pp

#endif // PP_LEDGER_FILE_DIR_STORE_H