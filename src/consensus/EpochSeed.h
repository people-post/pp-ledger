#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pp {
namespace consensus {

/** Lookback window for prior-epoch tip material (anti-grind). Protocol constant;
 * see docs/architecture/BLOCK_PIPELINE.md — not BlockChainConfig. */
inline constexpr size_t kEpochSeedLookback = 8;

/**
 * Epoch lottery seed helpers for SlotCommittee v2.
 * All digests are raw 32-byte SHA-256 (utl::sha256Raw).
 */
std::string deriveGenesisEpochSeed(const std::string &networkId,
                                   const std::string &genesisConfigDigest);

/** Fallback tip material when epoch E-1 produced no blocks. */
std::string emptyPrevEpochTipMaterial(uint64_t prevEpoch,
                                      const std::string &prevEpochSeed);

/**
 * Fold prior-epoch block hashes (oldest→newest, max kEpochSeedLookback)
 * into tip material. Empty hashes → emptyPrevEpochTipMaterial.
 */
std::string lookbackTipMaterial(uint64_t prevEpoch,
                                const std::string &prevEpochSeed,
                                const std::vector<std::string> &blockHashesOldestFirst);

std::string deriveEpochSeed(uint64_t epoch, const std::string &networkId,
                            const std::string &prevEpochSeed,
                            const std::string &tipMaterial,
                            const std::string &stakeSnapshotHash);

} // namespace consensus
} // namespace pp
