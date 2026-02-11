#ifndef PP_LEDGER_ACCOUNT_BUFFER_H
#define PP_LEDGER_ACCOUNT_BUFFER_H

#include "../client/Client.h"
#include "../lib/ResultOrError.hpp"
#include "../consensus/Ouroboros.h"

#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace pp {

/**
 * AccountBuffer - Manages user accounts in a buffer.
 */
class AccountBuffer {
public:
  // Well-known account IDs
  constexpr static uint64_t ID_GENESIS = 0;
  constexpr static uint64_t ID_FEE = 1;
  constexpr static uint64_t ID_RESERVE = 2;
  constexpr static uint64_t ID_RECYCLE = 3;

  constexpr static uint64_t ID_FIRST_USER = 1 << 20;

  constexpr static uint64_t INITIAL_TOKEN_SUPPLY = 1ULL << 30; // 1 billion tokens

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  constexpr static int32_t E_ACCOUNT = 1;
  constexpr static int32_t E_BALANCE = 2;
  constexpr static int32_t E_INPUT = 3;

  struct Account {
    uint64_t id{ 0 };
    Client::Wallet wallet;
    uint64_t blockId{ 0 }; // blockId of the last registration/renewal of the account
  };

  AccountBuffer();
  ~AccountBuffer() = default;

  bool isEmpty() const;
  bool hasAccount(uint64_t id) const;
  /** Returns account IDs whose blockId is strictly before the given blockId (account.blockId < blockId). */
  std::vector<uint64_t> getAccountIdsBeforeBlockId(uint64_t blockId) const;
  Roe<const Account&> getAccount(uint64_t id) const;
  int64_t getBalance(uint64_t accountId, uint64_t tokenId) const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  
  /** Verify if an account has sufficient spending power for a transaction.
   *  Checks both the transfer amount and fee balance requirements.
   *  Returns error if insufficient balances or invalid inputs. */
  Roe<void> verifySpendingPower(uint64_t accountId, uint64_t tokenId, int64_t amount, int64_t fee) const;

  Roe<void> add(const Account& account);

  Roe<void> update(const AccountBuffer& other);

  // Token-specific balance operations (tokenId: ID_GENESIS = native token, custom tokens use their genesis wallet IDs)
  Roe<void> depositBalance(uint64_t accountId, uint64_t tokenId, int64_t amount);
  Roe<void> transferBalance(uint64_t fromId, uint64_t toId, uint64_t tokenId, int64_t amount, int64_t fee = 0);
  Roe<void> withdrawBalance(uint64_t accountId, uint64_t tokenId, int64_t amount);
  
  /** Remove account by id. No-op if id does not exist. */
  void remove(uint64_t id);

  void clear();
  void reset();

private:

  bool isNegativeBalanceAllowed(const Account& account, uint64_t tokenId) const;

  std::map<uint64_t, Account> mAccounts_;
};

} // namespace pp

#endif // PP_LEDGER_ACCOUNT_BUFFER_H
