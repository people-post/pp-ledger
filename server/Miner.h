#pragma once

#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"

namespace pp {

class Miner : public Module {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };
    template <typename T> using Roe = ResultOrError<T, Error>;

    struct Config {
        std::string beaconAddress;
        uint64_t miningReward{ 0 };
        uint64_t miningDifficulty{ 0 };
        // Additional mining configuration parameters can be added here
    };

    Miner();
    virtual ~Miner() = default;
    Roe<void> init(const Config &config);
private:
    Config config_;
};

} // namespace pp