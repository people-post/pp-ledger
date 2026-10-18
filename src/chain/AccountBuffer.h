#ifndef PP_LEDGER_ACCOUNT_BUFFER_H
#define PP_LEDGER_ACCOUNT_BUFFER_H

#include "../client/AccountIds.h"
#include "../client/Client.h"
#include "../consensus/SlotCommittee.h"
#include "AccountStateTree.h"
#include "common/ResultOrError.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pp {

/**
 * AccountBuffer - committed account state for the chain tip.
 *
 * Maintains an incremental sparse Merkle tree updated only for touched
 * accounts. `calculateStateRoot()` is O(1). beginOverlay() starts an undo
 * journal (nestable) so a failed block or tx can be rolled back in
 * O(touched accounts).
 */
class AccountBuffer {
public:
  // Well-known account IDs (defined in client/AccountIds.h)
  constexpr static uint64_t ID_GENESIS = AccountIds::ID_GENESIS;
  constexpr static uint64_t ID_FEE = AccountIds::ID_FEE;
  constexpr static uint64_t ID_RESERVE = AccountIds::ID_RESERVE;
  constexpr static uint64_t ID_REGISTRAR = AccountIds::ID_REGISTRAR;
  constexpr static uint64_t ID_RECYCLE = AccountIds::ID_RECYCLE;
  constexpr static uint64_t ID_FIRST_ISSUED = AccountIds::ID_FIRST_ISSUED;
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
    /**
     * attachmentHashOf the account's attachment (`UserAccount.meta`) as its last write recorded it
     * (beacon-authority B031). The attachment itself stays in that record, in the block log; the
     * hash puts it under the state root.
     */
    std::string attachmentHash;
  };

  /** Raw SHA-256 committing to an attachment's bytes as recorded (B031). */
  static std::string attachmentHashOf(const std::string &attachment);

  AccountBuffer();
  ~AccountBuffer() = default;

  AccountBuffer(const AccountBuffer &) = delete;
  AccountBuffer &operator=(const AccountBuffer &) = delete;
  AccountBuffer(AccountBuffer &&) noexcept = default;
  AccountBuffer &operator=(AccountBuffer &&) noexcept = default;

  bool isEmpty() const;
  bool hasAccount(uint64_t id) const;
  /** Returns account IDs whose blockId is strictly before the given blockId
   * (account.blockId < blockId). */
  std::vector<uint64_t> getAccountIdsBeforeBlockId(uint64_t blockId) const;
  Roe<const Account &> getAccount(uint64_t id) const;
  int64_t getBalance(uint64_t accountId, uint64_t tokenId) const;
  /** Election candidates: registered miners (not system accounts) with a native balance of at
   *  least `minMinerStake` (and positive). */
  std::vector<consensus::Stakeholder> getStakeholders(uint64_t minMinerStake) const;

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
  /**
   * Replace an account's miner state in place (registration, liveness). Does not move the
   * account's blockId: renewal reads the account's meta from that block.
   */
  Roe<void> setMinerState(uint64_t accountId, const Client::MinerState &miner);
  /** Replace an account's keys and threshold in place (a recovery replacing the admin set, ba7). */
  Roe<void> setWalletKeys(uint64_t accountId, const std::vector<std::string> &publicKeys, uint8_t minSignatures);
  /** Account recovery state, in place (as setMinerState): the account's blockId does not move. */
  Roe<void> setRecovery(uint64_t accountId, const Client::AccountRecovery &recovery);
  /**
   * Every announcement due by `epoch` (applyEpoch <= epoch) applied, in id order: the account's keys
   * become the announced ones, the announcement cleared, its set and delay kept. The ids applied.
   */
  std::vector<uint64_t> applyDueRecoveries(uint64_t epoch);

  Roe<void> depositBalance(uint64_t accountId, uint64_t tokenId,
                           int64_t amount);
  Roe<void> transferBalance(uint64_t fromId, uint64_t toId, uint64_t tokenId,
                            uint64_t amount, uint64_t fee);
  Roe<void> withdrawBalance(uint64_t accountId, uint64_t tokenId,
                            int64_t amount);
  Roe<void> writeOff(uint64_t accountId);

  /** Remove account by id. No-op if id does not exist. */
  void remove(uint64_t id);

  /** Start recording undo entries for every mutation (nestable). */
  void beginOverlay();
  /** Keep changes since the matching beginOverlay(). */
  void commitOverlay();
  /** Revert every change since the matching beginOverlay(). */
  void rollbackOverlay();
  size_t overlayDepth() const { return journals_.size(); }

  void clear();
  void reset();

  /** (walletId, idempotentId) — the key BlockValidation allows once per block. */
  using IdempotencyKey = std::pair<uint64_t, uint64_t>;
  /**
   * Keys of the transactions applied to this buffer as a block draft. A block may carry each key
   * once; the draft enforces the same rule so a leader never assembles a block it cannot seal.
   * Journaled with the account state (an overlay rollback drops keys added inside it).
   */
  bool hasIdempotencyKey(const IdempotencyKey &key) const;
  void addIdempotencyKey(const IdempotencyKey &key);
  /**
   * Once-per-block markers for a draft (e.g. "one beacon certificate per block"): what block apply
   * would refuse a second time, the draft refuses too. Journaled and cleared like the keys above.
   */
  bool hasDraftTag(const std::string &tag) const;
  void addDraftTag(const std::string &tag);

  /**
   * O(1) root of the account sparse Merkle tree (incremental updates only).
   */
  std::string calculateStateRoot() const;

  /** Leaf digest for one account (used by the SMT). */
  static std::string accountLeafHash(const Account &account);
  /** The siblings along account `id`'s path in the state tree (AccountStateTree::siblings). */
  std::vector<std::string> stateSiblings(uint64_t id) const { return stateTree_.siblings(id); }

  /**
   * Account `accountId` issues token `tokenId`: its balance of it may go negative (minus that is the
   * supply out). The genesis account issues the native token; an issued account its own (token id =
   * account id). Users issue none.
   */
  static bool issuesToken(uint64_t accountId, uint64_t tokenId) {
    return accountId < ID_FIRST_USER && accountId == tokenId;
  }

private:
  bool isNegativeBalanceAllowed(const Account &account, uint64_t tokenId) const;
  void touchTree(const Account &account);
  void clearTree(uint64_t id);
  /**
   * Pointer into mAccounts_. ResultOrError cannot store references, so
   * mutators use Account* rather than Roe<Account&>.
   */
  Roe<Account *> mutableAccount(uint64_t id);
  /** Save the pre-mutation value of `id` into the innermost open journal. */
  void recordUndo(uint64_t id);

  struct Journal {
    AccountStateTree tree;
    /** Account value before first mutation; nullopt = did not exist. */
    std::map<uint64_t, std::optional<Account>> saved;
    /** Draft idempotency keys at beginOverlay() (a draft holds at most one block's worth). */
    std::set<IdempotencyKey> idempotencyKeys;
    std::set<std::string> draftTags;
  };

  std::map<uint64_t, Account> mAccounts_;
  AccountStateTree stateTree_;
  std::vector<Journal> journals_;
  std::set<IdempotencyKey> idempotencyKeys_;
  std::set<std::string> draftTags_;
};

} // namespace pp

#endif // PP_LEDGER_ACCOUNT_BUFFER_H
