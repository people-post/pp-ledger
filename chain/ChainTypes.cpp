#include "ChainTypes.h"
#include "lib/common/Utilities.h"

#include <sstream>

namespace pp {

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config) {
  os << "CheckpointConfig{minBlocks: " << config.minBlocks
     << ", minAgeSeconds: " << config.minAgeSeconds << "}";
  return os;
}

std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config) {
  os << "BlockChainConfig{genesisTime: " << config.genesisTime << ", "
     << "slotDuration: " << config.slotDuration << ", "
     << "slotsPerEpoch: " << config.slotsPerEpoch << ", "
     << "maxCustomMetaSize: " << config.maxCustomMetaSize << ", "
     << "maxTransactionsPerBlock: " << config.maxTransactionsPerBlock << ", "
     << "minFeeCoefficients: [" << utl::join(config.minFeeCoefficients, ", ")
     << "], "
     << "freeCustomMetaSize: " << config.freeCustomMetaSize << ", "
     << "checkpoint: " << config.checkpoint << ", "
     << "maxValidationTimespanSeconds: " << config.maxValidationTimespanSeconds
     << "}";
  return os;
}

std::string GenesisAccountMeta::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar &VERSION &*this;
  return oss.str();
}

bool GenesisAccountMeta::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar &version;
  if (version != VERSION) {
    return false;
  }
  ar &*this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

} // namespace pp
