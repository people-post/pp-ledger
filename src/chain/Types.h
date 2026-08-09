#ifndef PP_LEDGER_CHAIN_TYPES_H
#define PP_LEDGER_CHAIN_TYPES_H

#include "../client/Client.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace pp {

struct Checkpoint {
  uint64_t lastId{0};
  uint64_t currentId{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &lastId &currentId;
  }
};

struct CheckpointConfig {
  uint64_t minBlocks{0};
  uint64_t minAgeSeconds{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &minBlocks &minAgeSeconds;
  }
};

struct BlockChainConfig {
  int64_t genesisTime{0};
  uint64_t slotDuration{0};
  uint64_t slotsPerEpoch{0};
  uint64_t maxCustomMetaSize{0};
  uint64_t maxTransactionsPerBlock{0};
  std::vector<uint16_t> minFeeCoefficients;
  uint32_t freeCustomMetaSize{0};
  CheckpointConfig checkpoint;
  uint64_t maxValidationTimespanSeconds{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar &genesisTime &slotDuration &slotsPerEpoch &maxCustomMetaSize
        &maxTransactionsPerBlock &minFeeCoefficients &freeCustomMetaSize
            &checkpoint &maxValidationTimespanSeconds;
  }
};

struct GenesisAccountMeta {
  constexpr static const uint32_t VERSION = 1;

  BlockChainConfig config;
  Client::UserAccount genesis;

  template <typename Archive> void serialize(Archive &ar) {
    ar &config &genesis;
  }

  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);
};

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config);
std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config);

} // namespace pp

#endif
