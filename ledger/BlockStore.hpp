#ifndef PP_LEDGER_BLOCK_STORE_HPP
#define PP_LEDGER_BLOCK_STORE_HPP

#include "../lib/ResultOrError.hpp"
#include "Module.h"
#include <string>
#include <cstdint>

namespace pp {

class BlockStore : public Module {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T> using Roe = ResultOrError<T, Error>;

    BlockStore(const std::string &name) : Module(name) {}
    virtual ~BlockStore() = default;

    virtual bool canFit(uint64_t size) const = 0;

    uint64_t getLevel() const { return level_; }

    virtual uint64_t getBlockCount() const = 0;

    void setLevel(uint16_t level) { level_ = level; }

    virtual Roe<std::string> readBlock(uint64_t index) const = 0;
    virtual Roe<uint64_t> appendBlock(const std::string &block) = 0;

    virtual Roe<void> rewindTo(uint64_t index) = 0;

private:

    uint16_t level_{ 0 };
};

} // namespace pp

#endif // PP_LEDGER_BLOCK_STORE_HPP