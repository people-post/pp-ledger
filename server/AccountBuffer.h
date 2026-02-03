#ifndef PP_LEDGER_ACCOUNT_BUFFER_H
#define PP_LEDGER_ACCOUNT_BUFFER_H

#include "../lib/ResultOrError.hpp"

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

  constexpr static uint64_t ID_FIRST_USER = 1 << 20;

  constexpr static uint64_t INITIAL_TOKEN_SUPPLY = 1ULL << 30; // 1 billion tokens

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  struct Account {
    bool isNegativeBalanceAllowed{ false };
    uint64_t id{ 0 };
    std::vector<std::string> publicKeys;
    std::map<uint64_t, int64_t> mBalances; // tokenId -> balance (ID_GENESIS = native token)
  };

  AccountBuffer();
  ~AccountBuffer() = default;

  bool has(uint64_t id) const;
  Roe<const Account&> get(uint64_t id) const;
  uint64_t getTokenId() const;

  Roe<void> add(const Account& account);

  Roe<void> update(const AccountBuffer& other);

  // Token-specific balance operations (tokenId: ID_GENESIS = native token, custom tokens use their genesis wallet IDs)
  Roe<void> depositBalance(uint64_t accountId, uint64_t tokenId, int64_t amount);
  Roe<void> transferBalance(uint64_t fromId, uint64_t toId, uint64_t tokenId, int64_t amount);
  Roe<void> withdrawBalance(uint64_t accountId, uint64_t tokenId, int64_t amount);
  
  // Transaction processing: transfers amount of tokenId from fromId to toId, with fee in ID_GENESIS token
  // Creates toId account automatically if fromId has sufficient balance
  Roe<void> addTransaction(uint64_t fromId, uint64_t toId, uint64_t tokenId, int64_t amount, int64_t fee);
  
  // Helper to get balance for specific token (returns 0 if not found)
  int64_t getBalance(uint64_t accountId, uint64_t tokenId) const;

  /** Remove account by id. No-op if id does not exist. */
  void remove(uint64_t id);

  void clear();
  void reset(uint64_t tokenId);

private:
  std::map<uint64_t, Account> mAccounts_;
  uint64_t tokenId_{ 0 };
};

} // namespace pp

#endif // PP_LEDGER_ACCOUNT_BUFFER_H
