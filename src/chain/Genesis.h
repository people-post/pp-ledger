#pragma once
// Block 0 of a new network, as records: what `pp-beacon --init` signs with the genesis keys, or what
// an offline genesis ceremony signs (docs/contracts/OPERATION_FILE.md § Genesis).

#include "Types.h"
#include "../ledger/Ledger.h"
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
 * A miner account created in the genesis block, so the chain has real stake from block 0 (system
 * accounts never count as stake). Id in the issued range; only public keys (miners keep their keys).
 */
struct Miner {
  uint64_t id{0};
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{1};
  uint64_t stake{0}; ///< initial native-token balance, taken from the reserve
};

/** Everything block 0 says. */
struct Plan {
  /** With networkId and genesisTime (the network's slot 0). */
  BlockChainConfig config;
  SystemKeys keys;
  std::vector<Miner> miners;
  /** Recovery set and the generation-1 beacon certificate (its networkId is set from `config`). */
  ChainAuthority authority;
};

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
