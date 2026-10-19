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
 * that stands above the account's keys. It announces the account's next keys, which take over at the
 * first epoch start at least `delaySeconds` on; meanwhile the account is frozen (its keys sign nothing)
 * and only the set may withdraw it. The account's keys name the first set (T_NEW_USER, T_USER_UPDATE);
 * after that only the set changes itself (T_ACCOUNT_RECOVERY). Renewals carry all of it unchanged.
 */
struct AccountRecovery {
  /** Who may announce, and how many must sign; none: the account has no recovery. */
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{ 0 };
  /** Seconds from an announcement's slot to the earliest its keys take over (at least half an epoch). */
  uint64_t delaySeconds{ 0 };
  /** An announcement waiting: the account's next keys, the epoch they take over at, the block it is in. */
  bool pending{ false };
  std::vector<std::string> nextPublicKeys;
  uint8_t nextMinSignatures{ 0 };
  uint64_t applyEpoch{ 0 };
  uint64_t announcedAt{ 0 };

  bool hasSet() const { return !publicKeys.empty(); }
  bool operator==(const AccountRecovery &other) const = default;

  template <typename Archive> void serialize(Archive &ar) {
    ar & publicKeys & minSignatures & delaySeconds & pending & nextPublicKeys & nextMinSignatures & applyEpoch &
        announcedAt;
  }
};

/** What a T_ACCOUNT_RECOVERY does (its `meta`), signed by the account's recovery set. */
struct AccountRecoveryAction {
  constexpr static const uint32_t VERSION = 1;

  enum Kind : uint8_t {
    /** The account's next keys (`publicKeys`, `minSignatures`): the account frozen until they apply. */
    ANNOUNCE = 1,
    /** The announcement waiting withdrawn: the account's keys sign again. */
    WITHDRAW = 2,
    /** The recovery set replaced (`publicKeys`, `minSignatures`, `delaySeconds`), or removed (no keys). */
    CHANGE = 3,
  };

  uint8_t kind{ 0 };
  std::vector<std::string> publicKeys;
  uint8_t minSignatures{ 0 };
  uint64_t delaySeconds{ 0 };

  template <typename Archive> void serialize(Archive &ar) { ar & kind & publicKeys & minSignatures & delaySeconds; }

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
  /**
   * The account's domain (projects/domains): its symbol, `A`–`Z`, 1–3 letters for a system account,
   * 4–7 for an issued one; none for a user. Given by the genesis block or the registrar, then
   * chain-owned and never changed; free again when the account ends.
   */
  std::string domain;

  bool operator==(const Wallet& other) const {
    return mBalances == other.mBalances &&
           publicKeys == other.publicKeys &&
           minSignatures == other.minSignatures &&
           keyType == other.keyType &&
           miner == other.miner &&
           recovery == other.recovery &&
           domain == other.domain;
  }

  template <typename Archive> void serialize(Archive &ar) {
    ar & mBalances & publicKeys & minSignatures & keyType & miner & recovery & domain;
  }

  pp::common::Meta ltsToMeta() const;
};

struct UserAccount {
  constexpr static const uint32_t VERSION = 4;

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
