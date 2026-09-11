#include "EpochSeed.h"
#include "common/Serialize.hpp"
#include "lib/common/Utilities.h"
#include <sstream>

namespace pp {
namespace consensus {

std::string deriveGenesisEpochSeed(const std::string &networkId,
                                   const std::string &genesisConfigDigest) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  const std::string domain = "pp-ledger/epoch-seed/genesis/v1";
  oss.write(domain.data(), static_cast<std::streamsize>(domain.size()));
  uint64_t n = static_cast<uint64_t>(networkId.size());
  ar & n;
  oss.write(networkId.data(), static_cast<std::streamsize>(networkId.size()));
  oss.write(genesisConfigDigest.data(),
            static_cast<std::streamsize>(genesisConfigDigest.size()));
  const std::string zeros = utl::zeroHash();
  oss.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  return utl::sha256Raw(oss.str());
}

std::string emptyPrevEpochTipMaterial(uint64_t prevEpoch,
                                      const std::string &prevEpochSeed) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  const std::string domain = "pp-ledger/epoch-seed/empty-prev/v1";
  oss.write(domain.data(), static_cast<std::streamsize>(domain.size()));
  ar & prevEpoch;
  oss.write(prevEpochSeed.data(),
            static_cast<std::streamsize>(prevEpochSeed.size()));
  return utl::sha256Raw(oss.str());
}

std::string lookbackTipMaterial(uint64_t prevEpoch,
                                const std::string &prevEpochSeed,
                                const std::vector<std::string> &blockHashesOldestFirst) {
  if (blockHashesOldestFirst.empty()) {
    return emptyPrevEpochTipMaterial(prevEpoch, prevEpochSeed);
  }
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  const std::string domain = "pp-ledger/epoch-seed/lookback/v1";
  oss.write(domain.data(), static_cast<std::streamsize>(domain.size()));
  uint64_t k = static_cast<uint64_t>(blockHashesOldestFirst.size());
  ar & k;
  for (const auto &h : blockHashesOldestFirst) {
    oss.write(h.data(), static_cast<std::streamsize>(h.size()));
  }
  return utl::sha256Raw(oss.str());
}

std::string deriveEpochSeed(uint64_t epoch, const std::string &networkId,
                            const std::string &prevEpochSeed,
                            const std::string &tipMaterial,
                            const std::string &stakeSnapshotHash) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  const std::string domain = "pp-ledger/epoch-seed/v1";
  oss.write(domain.data(), static_cast<std::streamsize>(domain.size()));
  ar & epoch;
  uint64_t n = static_cast<uint64_t>(networkId.size());
  ar & n;
  oss.write(networkId.data(), static_cast<std::streamsize>(networkId.size()));
  oss.write(prevEpochSeed.data(),
            static_cast<std::streamsize>(prevEpochSeed.size()));
  oss.write(tipMaterial.data(),
            static_cast<std::streamsize>(tipMaterial.size()));
  oss.write(stakeSnapshotHash.data(),
            static_cast<std::streamsize>(stakeSnapshotHash.size()));
  return utl::sha256Raw(oss.str());
}

} // namespace consensus
} // namespace pp
