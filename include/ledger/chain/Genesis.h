#pragma once
// Block 0 of a new network, as records: what `pp-beacon --init` signs with the genesis keys, or what
// an offline genesis ceremony signs (docs/contracts/OPERATION_FILE.md § Genesis).

#include "ledger/chain/Types.h"
#include "ledger/ledger/Ledger.h"
#include "common/ResultOrError.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pp::chain_genesis {

/** An account's keys: raw ML-DSA-65 public keys; minSignatures 0 means all of them. */
struct AccountKeys {
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{0};
};

/** The system accounts' keys (init-config.json `systemAccounts`). */
struct SystemKeys {
  AccountKeys genesis;
  AccountKeys reserve;
  AccountKeys registrar;
  AccountKeys fee;
  AccountKeys recycle;
};

/**
 * The system accounts' domains (projects/domains; init-config.json `systemNames`): 1–3 capital
 * letters each, distinct; empty for none. The genesis account's is the native token's symbol.
 */
struct SystemNames {
  std::string genesis;
  std::string reserve;
  std::string registrar;
  std::string fee;
  std::string recycle;
};

/**
 * A miner account created in the genesis block, so the chain has real stake from block 0 (system
 * accounts never count as stake). Id in the issued range; only public keys (miners keep their keys).
 */
struct Miner {
  uint64_t id{0};
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{1};
  uint64_t stake{0}; ///< initial native-token balance, taken from the reserve
};

/** The provisional system names (projects/domains DN006): PPL, RSV, REG, FEE, RCY. */
SystemNames defaultSystemNames();
/** Each name 1–3 capital letters (or none), no two the same. */
pp::Roe<void> checkNames(const SystemNames &names);

/** Everything block 0 says. */
struct Plan {
  /** With networkId and genesisTime (the network's slot 0). */
  BlockChainConfig config;
  SystemKeys keys;
  SystemNames names = defaultSystemNames();
  std::vector<Miner> miners;
  /** Recovery set and the generation-1 beacon certificate (its networkId is set from `config`). */
  ChainAuthority authority;
};

/** What a new network starts with unless its plan says otherwise (pp-beacon --init, the Genesis app). */
namespace defaults {
inline constexpr uint64_t kSlotDuration = 7;                                       ///< seconds
inline constexpr uint64_t kSlotsPerEpoch = 24ULL * 3600;                           ///< 7 days of 7 s slots
inline constexpr uint64_t kMaxCustomMetaSize = 1ULL * 1024 * 1024;                 ///< 1 MiB
inline constexpr uint64_t kMaxTransactionsPerBlock = 10ULL * 1024;
inline constexpr uint64_t kMaxBlockBytes = 4ULL * 1024 * 1024;                     ///< 4 MiB (B034)
inline constexpr uint16_t kMinFeeCoefficients[3] = {1, 1, 1};                     ///< a + b·KiB + c·KiB²
inline constexpr uint64_t kFreeCustomMetaSize = 1024;
inline constexpr uint64_t kCheckpointMinBlocks = 1ULL << 20;
inline constexpr uint64_t kCheckpointMinAgeSeconds = 365ULL * 24 * 3600;
inline constexpr uint64_t kMaxValidationTimespanSeconds = 86400;
inline constexpr uint64_t kMinerDueSeconds = 86400;                                ///< a day of slots
inline constexpr uint64_t kMinerMaxMissedSlots = 3;
inline constexpr uint64_t kRecoveryDelaySeconds = 30ULL * 24 * 60 * 60;            ///< about a month
inline constexpr uint64_t kFinalitySeconds = 24ULL * 3600;                         ///< a day (FN001)
/** Genesis miners without a stake share 1/kGenesisMinerStakeShare of the supply. */
inline constexpr uint64_t kGenesisMinerStakeShare = 10;
} // namespace defaults

/**
 * The default config of network `networkId`: the defaults above; heartbeats every epoch, miners due
 * after a day of slots, no minting until a config update allows it, no flat new-account fee.
 */
BlockChainConfig defaultConfig(const std::string &networkId);
/** About kRecoveryDelaySeconds of `config`'s epochs, at least 1. */
uint64_t defaultRecoveryDelayEpochs(const BlockChainConfig &config);
/** Each of `withoutStake` genesis miners' default stake. */
uint64_t defaultMinerStake(size_t withoutStake);

/** An account of `keys` holding `balance` of the native token. */
UserAccount accountOf(const AccountKeys &keys, int64_t balance, const std::string &meta);

/** Every system account has well-formed keys; the genesis (admin) account is M-of-N. */
pp::Roe<void> checkKeys(const SystemKeys &keys);

/**
 * Block 0's records, unsigned: T_GENESIS (config, admin account, authority), then the fee, reserve,
 * registrar and recycle accounts, then the miners. The reserve holds the supply less the others'
 * stakes and fees. Every record is signed by the genesis account's keys.
 */
pp::Roe<std::vector<Ledger::Record>> records(const Plan &plan);

/** The genesis meta of a genesis record (T_GENESIS). */
pp::Roe<GenesisAccountMeta> metaOf(const Ledger::Record &genesisRecord);

} // namespace pp::chain_genesis
