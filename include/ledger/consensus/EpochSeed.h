#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pp {
namespace consensus {

/** Most consecutive epochs without a block a seed derives through; a chain idle longer stops. */
inline constexpr uint64_t kMaxEmptyEpochs = 1024;
/** Epochs a beacon seed hash chain covers by default (one reveal per epoch). */
inline constexpr uint64_t kDefaultBeaconSeedChainLength = 10000;
/** Longest chain a beacon certificate may declare; bounds verification work. */
inline constexpr uint64_t kMaxBeaconSeedChainLength = 100000;

/**
 * Epoch lottery seed helpers for SlotCommittee v2.
 * All digests are raw 32-byte SHA-256 (utl::sha256Raw).
 */
std::string deriveGenesisEpochSeed(const std::string& networkId, const std::string& genesisConfigDigest);

/** One beacon hash-chain step: sha256Raw("pp-ledger/beacon-seed/v1" || value). */
std::string beaconSeedStep(const std::string& value);

/** H^steps(secret); steps == 0 returns secret unchanged. */
std::string beaconSeedValue(const std::string& secret, uint64_t steps);

/** True iff reveal is 32 bytes and H^steps(reveal) == anchor. steps == 0 is refused. */
bool verifyBeaconSeedReveal(const std::string& anchor, uint64_t steps, const std::string& reveal);

/**
 * Epoch seed v2: mixes the beacon's hash-chain reveal (nullopt when the beacon
 * has none) into the previous seed, so block producers cannot grind it.
 */
std::string deriveEpochSeedV2(uint64_t epoch, const std::string& networkId, const std::string& prevEpochSeed,
                              const std::optional<std::string>& beaconReveal);

} // namespace consensus
} // namespace pp
