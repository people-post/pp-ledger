#pragma once
// Account state as the chain records it: balances, keys and threshold, miner liveness. Pure data —
// read and built without any network code (operation files, admin tools); Client keeps the names
// (Client::Wallet, Client::UserAccount, Client::MinerState).

#include "lib/common/Meta.h"

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace pp {

/**
 * Miner registration and liveness (docs/contracts/WIRE_SCHEMA.md).
 * Chain-owned: only T_MINER_REGISTER / T_MINER_REVOKE and block apply change it; account
 * creation starts it empty (genesis miners registered) and user updates / renewals must
 * carry it unchanged.
 */
struct MinerState {
  constexpr static uint64_t kNever = UINT64_MAX;

  bool registered{ false };
  /** Slot of its last block (or of its registration); it is due once this is old enough. */
  uint64_t lastProducedSlot{ 0 };
  /** Consecutive slots it led while due and produced nothing. */
  uint32_t missedDue{ 0 };
  /** Epoch it was deregistered by the liveness rule (cool-down); kNever if never. */
  uint64_t autoDeregisteredEpoch{ kNever };

  bool operator==(const MinerState& other) const = default;

  template <typename Archive> void serialize(Archive &ar) {
    ar & registered & lastProducedSlot & missedDue & autoDeregisteredEpoch;
  }
};

struct Wallet {
  std::map<uint64_t, int64_t> mBalances; // tokenId -> balance
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{ 0 };
  uint8_t keyType{ 0 };  // Crypto::TK_ML_DSA_65 = 1; use Crypto::isSupported() to check
  MinerState miner;

  bool operator==(const Wallet& other) const {
    return mBalances == other.mBalances &&
           publicKeys == other.publicKeys &&
           minSignatures == other.minSignatures &&
           keyType == other.keyType &&
           miner == other.miner;
  }

  template <typename Archive> void serialize(Archive &ar) {
    ar & mBalances & publicKeys & minSignatures & keyType & miner;
  }

  pp::common::Meta ltsToMeta() const;
};

struct UserAccount {
  constexpr static const uint32_t VERSION = 2;

  Wallet wallet;
  std::string meta;

  bool operator==(const UserAccount& other) const {
    return wallet == other.wallet && meta == other.meta;
  }

  template <typename Archive> void serialize(Archive &ar) {
    ar & wallet & meta;
  }

  std::string ltsToString() const;
  bool ltsFromString(const std::string& str);
  pp::common::Meta ltsToMeta() const;
};

std::ostream &operator<<(std::ostream &os, const Wallet &wallet);
std::ostream &operator<<(std::ostream &os, const UserAccount &account);

} // namespace pp
