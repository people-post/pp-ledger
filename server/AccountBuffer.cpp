#include "AccountBuffer.h"

namespace pp {

AccountBuffer::AccountBuffer() {
}

bool AccountBuffer::has(uint64_t id) const {
  return mAccounts_.find(id) != mAccounts_.end();
}

uint64_t AccountBuffer::getTokenId() const {
  return tokenId_;
}

AccountBuffer::Roe<const AccountBuffer::Account&> AccountBuffer::get(uint64_t id) const {
  auto it = mAccounts_.find(id);
  if (it == mAccounts_.end()) {
    return Error(1, "Account not found");
  }
  return it->second;
}

int64_t AccountBuffer::getBalance(uint64_t accountId, uint64_t tokenId) const {
  auto it = mAccounts_.find(accountId);
  if (it == mAccounts_.end()) {
    return 0;
  }
  auto balanceIt = it->second.mBalances.find(tokenId);
  if (balanceIt == it->second.mBalances.end()) {
    return 0;
  }
  return balanceIt->second;
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

AccountBuffer::Roe<void> AccountBuffer::depositBalance(uint64_t accountId, uint64_t tokenId, int64_t amount) {
  if (amount < 0) {
    return Error(10, "Deposit amount must be non-negative");
  }

  auto it = mAccounts_.find(accountId);
  if (it == mAccounts_.end()) {
    return Error(9, "Account not found");
  }
  
  int64_t currentBalance = 0;
  auto balanceIt = it->second.mBalances.find(tokenId);
  if (balanceIt != it->second.mBalances.end()) {
    currentBalance = balanceIt->second;
  }
  
  if (currentBalance > INT64_MAX - amount) {
    return Error(11, "Deposit would cause balance overflow");
  }
  it->second.mBalances[tokenId] = currentBalance + amount;
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::withdrawBalance(uint64_t accountId, uint64_t tokenId, int64_t amount) {
  if (amount < 0) {
    return Error(11, "Withdraw amount must be non-negative");
  }

  auto it = mAccounts_.find(accountId);
  if (it == mAccounts_.end()) {
    return Error(12, "Account not found");
  }
  
  int64_t currentBalance = 0;
  auto balanceIt = it->second.mBalances.find(tokenId);
  if (balanceIt != it->second.mBalances.end()) {
    currentBalance = balanceIt->second;
  }
  
  if (!it->second.isNegativeBalanceAllowed && currentBalance < amount) {
    return Error(13, "Insufficient balance");
  }
  if (currentBalance < INT64_MIN + amount) {
    return Error(14, "Withdraw would cause balance underflow");
  }
  it->second.mBalances[tokenId] = currentBalance - amount;
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::transferBalance(uint64_t fromId, uint64_t toId, uint64_t tokenId, int64_t amount) {
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

  int64_t fromBalance = 0;
  auto fromBalanceIt = fromIt->second.mBalances.find(tokenId);
  if (fromBalanceIt != fromIt->second.mBalances.end()) {
    fromBalance = fromBalanceIt->second;
  }
  
  int64_t toBalance = 0;
  auto toBalanceIt = toIt->second.mBalances.find(tokenId);
  if (toBalanceIt != toIt->second.mBalances.end()) {
    toBalance = toBalanceIt->second;
  }

  // Check if source account has sufficient balance (unless negative balance is allowed)
  if (!fromIt->second.isNegativeBalanceAllowed && fromBalance < amount) {
    return Error(6, "Insufficient balance");
  }

  // Check for overflow in destination account
  if (toBalance > INT64_MAX - amount) {
    return Error(7, "Transfer would cause balance overflow");
  }

  // Perform the transfer
  fromIt->second.mBalances[tokenId] = fromBalance - amount;
  toIt->second.mBalances[tokenId] = toBalance + amount;

  return {};
}

void AccountBuffer::remove(uint64_t id) {
  mAccounts_.erase(id);
}

void AccountBuffer::clear() {
  mAccounts_.clear();
}

void AccountBuffer::reset(uint64_t tokenId) {
  clear();
  tokenId_ = tokenId;
}

} // namespace pp
