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

/**
 * An account's way back (projects/account-recovery; WIRE_SCHEMA § Account recovery): a recovery set
 * the owner chooses may announce the account's next keys, which take over `delayEpochs` later unless
 * the owner's own update cancels them first. The set and delay are the owner's (T_NEW_USER,
 * T_USER_UPDATE); the announcement is chain-owned (T_ACCOUNT_RECOVERY sets it, an update clears it,
 * block apply applies it); renewals carry all of it unchanged.
 */
struct AccountRecovery {
  /** Who may announce, and how many must sign; none: the account has no recovery. */
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{ 0 };
  /** Epochs between an announcement and its taking over (at least 1). */
  uint64_t delayEpochs{ 0 };
  /** An announcement waiting: the account's next keys, when they take over, the block it is in. */
  bool pending{ false };
  std::vector<std::string> nextPublicKeys;
  uint8_t nextMinSignatures{ 0 };
  uint64_t applyEpoch{ 0 };
  uint64_t announcedAt{ 0 };

  bool hasSet() const { return !publicKeys.empty(); }
  bool operator==(const AccountRecovery &other) const = default;

  template <typename Archive> void serialize(Archive &ar) {
    ar & publicKeys & minSignatures & delayEpochs & pending & nextPublicKeys & nextMinSignatures & applyEpoch &
        announcedAt;
  }
};

/** What a T_ACCOUNT_RECOVERY announces: the account's next keys and threshold (its `meta`). */
struct AccountRecoveryAnnouncement {
  constexpr static const uint32_t VERSION = 1;

  std::vector<std::string> publicKeys;
  uint8_t minSignatures{ 0 };

  template <typename Archive> void serialize(Archive &ar) { ar & publicKeys & minSignatures; }

  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);
};

struct Wallet {
  std::map<uint64_t, int64_t> mBalances; // tokenId -> balance
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{ 0 };
  uint8_t keyType{ 0 };  // Crypto::TK_ML_DSA_65 = 1; use Crypto::isSupported() to check
  MinerState miner;
  AccountRecovery recovery;

  bool operator==(const Wallet& other) const {
    return mBalances == other.mBalances &&
           publicKeys == other.publicKeys &&
           minSignatures == other.minSignatures &&
           keyType == other.keyType &&
           miner == other.miner &&
           recovery == other.recovery;
  }

  template <typename Archive> void serialize(Archive &ar) {
    ar & mBalances & publicKeys & minSignatures & keyType & miner & recovery;
  }

  pp::common::Meta ltsToMeta() const;
};

struct UserAccount {
  constexpr static const uint32_t VERSION = 3;

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
