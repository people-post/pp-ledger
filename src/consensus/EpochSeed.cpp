#include "ledger/consensus/EpochSeed.h"
#include "common/Serialize.hpp"
#include "ledger/lib/common/Utilities.h"
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

std::string beaconSeedStep(const std::string &value) {
  return utl::sha256Raw("pp-ledger/beacon-seed/v1" + value);
}

std::string beaconSeedValue(const std::string &secret, uint64_t steps) {
  std::string v = secret;
  for (uint64_t i = 0; i < steps; ++i) {
    v = beaconSeedStep(v);
  }
  return v;
}

bool verifyBeaconSeedReveal(const std::string &anchor, uint64_t steps,
                            const std::string &reveal) {
  if (reveal.size() != 32 || steps == 0) {
    return false;
  }
  return beaconSeedValue(reveal, steps) == anchor;
}

std::string deriveEpochSeedV2(uint64_t epoch, const std::string &networkId,
                              const std::string &prevEpochSeed,
                              const std::optional<std::string> &beaconReveal) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  const std::string domain = "pp-ledger/epoch-seed/v2";
  oss.write(domain.data(), static_cast<std::streamsize>(domain.size()));
  ar & epoch;
  uint64_t n = static_cast<uint64_t>(networkId.size());
  ar & n;
  oss.write(networkId.data(), static_cast<std::streamsize>(networkId.size()));
  oss.write(prevEpochSeed.data(),
            static_cast<std::streamsize>(prevEpochSeed.size()));
  uint8_t present = beaconReveal.has_value() ? 1 : 0;
  ar & present;
  if (beaconReveal) {
    oss.write(beaconReveal->data(),
              static_cast<std::streamsize>(beaconReveal->size()));
  }
  return utl::sha256Raw(oss.str());
}

} // namespace consensus
} // namespace pp
