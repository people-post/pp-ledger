#pragma once

#include "../ledger/Ledger.h"
#include "../ledger/Block.h"
#include "../ledger/BlockChain.h"
#include "../consensus/Ouroboros.h"
#include "../network/Types.hpp"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"

#include <string>
#include <cstdint>

namespace pp {

class Miner : public Module {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };
    template <typename T> using Roe = ResultOrError<T, Error>;

    struct Config {
        std::string workDir;
    };

    Miner();
    virtual ~Miner() = default;
    Roe<void> init(const Config &config);

    // Check if this node should produce a block in the current slot
    bool shouldProduceBlock() const;

    // Produce a block if eligible
    Roe<void> produceBlock();

    Roe<void> addTransaction(const Ledger::Transaction &tx);

    uint64_t getCurrentBlockId() const;

    Roe<void> syncChain(const BlockChain& chain);

    Roe<void> addBlock(const std::string& data);

private:
    Config config_;
    consensus::Ouroboros consensus_;
    Ledger ledger_;
};

} // namespace pp