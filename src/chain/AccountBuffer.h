#ifndef PP_LEDGER_ACCOUNT_BUFFER_H
#define PP_LEDGER_ACCOUNT_BUFFER_H

#include "../client/AccountIds.h"
#include "../client/Client.h"
#include "../consensus/Ouroboros.h"
#include "AccountStateTree.h"
#include "common/ResultOrError.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pp {

/**
 * AccountBuffer - Manages user accounts in a buffer.
 *
 * Maintains an incremental sparse Merkle tree (`stateTree_`) updated only for
 * touched accounts. `calculateStateRoot()` is O(1).
 *
 * `forkForSeal()` builds a cheap overlay that reads through to this buffer and
 * clones the SMT (O(1)); seal dry-runs mutate only touched accounts / paths.
 */
class AccountBuffer {
public:
  // Well-known account IDs (defined in client/AccountIds.h)
  constexpr static uint64_t ID_GENESIS = AccountIds::ID_GENESIS;
  constexpr static uint64_t ID_FEE = AccountIds::ID_FEE;
  constexpr static uint64_t ID_RESERVE = AccountIds::ID_RESERVE;
  constexpr static uint64_t ID_RECYCLE = AccountIds::ID_RECYCLE;
  constexpr static uint64_t ID_FIRST_USER = AccountIds::ID_FIRST_USER;
  constexpr static uint64_t INITIAL_TOKEN_SUPPLY =
      AccountIds::INITIAL_TOKEN_SUPPLY;

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  constexpr static int32_t E_ACCOUNT = 1;
  constexpr static int32_t E_BALANCE = 2;
  constexpr static int32_t E_INPUT = 3;

  struct Account {
    uint64_t id{0};
    Client::Wallet wallet;
    uint64_t blockId{
        0}; // blockId of the last registration/renewal of the account
  };

  AccountBuffer();
  ~AccountBuffer() = default;

  AccountBuffer(const AccountBuffer &) = delete;
  AccountBuffer &operator=(const AccountBuffer &) = delete;
  AccountBuffer(AccountBuffer &&) noexcept = default;
  AccountBuffer &operator=(AccountBuffer &&) noexcept = default;

  /**
   * Overlay fork for seal dry-run: reads fall through to `*this`, writes stay
   * local, SMT is path-copied from this buffer's tree.
   */
  AccountBuffer forkForSeal() const;

  bool isEmpty() const;
  bool hasAccount(uint64_t id) const;
  /** Returns account IDs whose blockId is strictly before the given blockId
   * (account.blockId < blockId). */
  std::vector<uint64_t> getAccountIdsBeforeBlockId(uint64_t blockId) const;
  Roe<const Account &> getAccount(uint64_t id) const;
  int64_t getBalance(uint64_t accountId, uint64_t tokenId) const;
  std::vector<consensus::Stakeholder> getStakeholders() const;

  /** Verify if an account has sufficient spending power for a transaction.
   *  Checks both the transfer amount and fee balance requirements.
   *  Returns error if insufficient balances or invalid inputs. */
  Roe<void> verifySpendingPower(uint64_t accountId, uint64_t tokenId,
                                uint64_t amount, uint64_t fee) const;

  /** Verify that after applying amount and fee, the account balance in buffer
   * exactly matches given balance map. For non-genesis tokens, balances must
   * match exactly. For genesis token, buffer balance should equal given balance
   * plus amount and fee (buffer = given + amount + fee). This is used to
   * validate that the buffer state before a transaction matches the expected
   * state after accounting for the transaction's amount and fee. Returns error
   * if balances don't match. */
  Roe<void>
  verifyBalance(uint64_t accountId, uint64_t amount, uint64_t fee,
                const std::map<uint64_t, int64_t> &expectedBalances) const;

  Roe<void> add(const Account &account);

  Roe<void> update(const AccountBuffer &other);

  /** Ensure `accountId` exists in this buffer by copying it from `committed`
   * when missing. Returns TxError if the account is missing from committed or
   * cannot be inserted into this buffer. */
  Roe<void> seedFromCommittedIfMissing(const AccountBuffer &committed,
                                       uint64_t accountId);

  // Token-specific balance operations (tokenId: ID_GENESIS = native token,
  // custom tokens use their genesis wallet IDs)
  Roe<void> depositBalance(uint64_t accountId, uint64_t tokenId,
                           int64_t amount);
  Roe<void> transferBalance(uint64_t fromId, uint64_t toId, uint64_t tokenId,
                            uint64_t amount, uint64_t fee);
  Roe<void> withdrawBalance(uint64_t accountId, uint64_t tokenId,
                            int64_t amount);
  Roe<void> writeOff(uint64_t accountId);

  /** Remove account by id. No-op if id does not exist. */
  void remove(uint64_t id);

  void clear();
  void reset();

  /**
   * O(1) root of the account sparse Merkle tree (incremental updates only).
   */
  std::string calculateStateRoot() const;

  /** Leaf digest for one account (used by the SMT). */
  static std::string accountLeafHash(const Account &account);

private:
  bool isNegativeBalanceAllowed(const Account &account, uint64_t tokenId) const;
  bool isDeleted(uint64_t id) const;
  void touchTree(const Account &account);
  void clearTree(uint64_t id);
  /** Materialize account into local map for mutation (overlay CoW).
   *  Returns a pointer into mAccounts_. ResultOrError cannot store references. */
  Roe<Account *> mutableAccount(uint64_t id);

  /** Local accounts (full store, or overlay deltas when parent_ != nullptr). */
  std::map<uint64_t, Account> mAccounts_;
  /** Ids removed in an overlay that still exist on parent_. */
  std::set<uint64_t> deleted_;
  const AccountBuffer *parent_{nullptr};
  AccountStateTree stateTree_;
};

} // namespace pp

#endif // PP_LEDGER_ACCOUNT_BUFFER_H
