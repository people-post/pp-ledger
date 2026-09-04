#ifndef PP_LEDGER_DIR_STORE_H
#define PP_LEDGER_DIR_STORE_H

#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include <cstdint>
#include <string>

namespace pp {

/**
 * DirStore is an abstract base for directory-based block stores
 * (FileDirStore, VolumeStore).
 */
class DirStore : public Module {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T> using Roe = ResultOrError<T, Error>;

    static constexpr uint32_t MAGIC_FILE_DIR = 0x504C4944; // "PLID"
    static constexpr uint32_t MAGIC_VOLUMES = 0x504C564F;  // "PLVO"

    DirStore() = default;
    ~DirStore() override = default;

    virtual bool canFit(uint64_t size) const = 0;
    virtual uint64_t getBlockCount() const = 0;
    virtual uint64_t countSizeFromBlockId(uint64_t blockId) const = 0;
    virtual Roe<std::string> readBlock(uint64_t index) const = 0;
    virtual Roe<uint64_t> appendBlock(const std::string &block) = 0;
    virtual Roe<void> rewindTo(uint64_t index) = 0;

protected:
    /** Format an ID as a zero-padded 6-digit string (e.g. 1 -> "000001"). */
    static std::string formatId(uint32_t id);

    static std::string getIndexFilePath(const std::string &dirPath);

    Roe<void> ensureDirectory(const std::string &dirPath) const;
    Roe<void> validateMinFileSize(size_t maxFileSize) const;
};

} // namespace pp

#endif // PP_LEDGER_DIR_STORE_H
