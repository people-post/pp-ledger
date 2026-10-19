#ifndef PP_LEDGER_RELAY_H
#define PP_LEDGER_RELAY_H

#include "FollowerCore.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace pp {

/**
 * Relay - a full copy of the chain from genesis, served downstream.
 *
 * It follows its upstream (FollowerCore) and adds nothing to the chain itself; RelayServer serves
 * it and forwards writes upstream.
 */
class Relay : public FollowerCore {
public:
  struct InitConfig {
    std::string workDir;
    int64_t timeOffsetMs{0};
    uint64_t startingBlockId{0};
  };

  Relay();
  ~Relay() override = default;

  Roe<void> init(const InitConfig &config);
};

// Ostream operators for easy logging
std::ostream &operator<<(std::ostream &os, const Relay::InitConfig &config);

} // namespace pp

#endif // PP_LEDGER_RELAY_H
