#ifndef PP_LEDGER_ACCOUNT_BUFFER_H
#define PP_LEDGER_ACCOUNT_BUFFER_H

#include "../lib/ResultOrError.hpp"

#include <map>
#include <string>
#include <cstdint>

namespace pp {

/**
 * AccountBuffer - Manages user accounts in a buffer.
 */
class AccountBuffer {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  struct Account {
    bool isNegativeBalanceAllowed{ false };
    uint64_t id{ 0 };
    std::string publicKey;
    int64_t balance{ 0 };
  };

  AccountBuffer();
  ~AccountBuffer() = default;

  bool has(uint64_t id) const;
  Roe<const Account&> get(uint64_t id);

  Roe<void> add(const Account& account);

  Roe<void> update(const AccountBuffer& other);

  Roe<void> transferBalance(uint64_t fromId, uint64_t toId, int64_t amount);

  /** Remove account by id. No-op if id does not exist. */
  void remove(uint64_t id);

  void clear();

private:
  std::map<uint64_t, Account> mAccounts_;
};

} // namespace pp

#endif // PP_LEDGER_ACCOUNT_BUFFER_H
