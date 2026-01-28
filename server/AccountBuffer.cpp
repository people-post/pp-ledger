#include "AccountBuffer.h"

namespace pp {

AccountBuffer::AccountBuffer() {
}

bool AccountBuffer::has(uint64_t id) const {
  return mAccounts_.find(id) != mAccounts_.end();
}

AccountBuffer::Roe<const AccountBuffer::Account&> AccountBuffer::get(uint64_t id) {
  auto it = mAccounts_.find(id);
  if (it == mAccounts_.end()) {
    return Error(1, "Account not found");
  }
  return it->second;
}

AccountBuffer::Roe<void> AccountBuffer::add(const Account& account) {
  if (has(account.id)) {
    return Error(2, "Account already exists");
  }
  mAccounts_[account.id] = account;
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::update(const AccountBuffer& other) {
  for (const auto& [id, account] : other.mAccounts_) {
    if (!has(id)) {
      return Error(8, "Account to update not found: " + std::to_string(id));
    }
    mAccounts_[id] = account;
  }
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::transferBalance(uint64_t fromId, uint64_t toId, int64_t amount) {
  if (amount < 0) {
    return Error(3, "Transfer amount must be non-negative");
  }

  auto fromIt = mAccounts_.find(fromId);
  if (fromIt == mAccounts_.end()) {
    return Error(4, "Source account not found");
  }

  auto toIt = mAccounts_.find(toId);
  if (toIt == mAccounts_.end()) {
    return Error(5, "Destination account not found");
  }

  // Check if source account has sufficient balance (unless negative balance is allowed)
  if (!fromIt->second.isNegativeBalanceAllowed && fromIt->second.balance < amount) {
    return Error(6, "Insufficient balance");
  }

  // Check for overflow in destination account
  if (toIt->second.balance > INT64_MAX - amount) {
    return Error(7, "Transfer would cause balance overflow");
  }

  // Perform the transfer
  fromIt->second.balance -= amount;
  toIt->second.balance += amount;

  return {};
}

void AccountBuffer::clear() {
  mAccounts_.clear();
}

} // namespace pp
