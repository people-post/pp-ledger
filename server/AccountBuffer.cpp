#include "AccountBuffer.h"

namespace pp {

AccountBuffer::AccountBuffer() {
}

bool AccountBuffer::isEmpty() const {
  return mAccounts_.empty();
}

bool AccountBuffer::hasAccount(uint64_t id) const {
  return mAccounts_.find(id) != mAccounts_.end();
}

std::vector<uint64_t> AccountBuffer::getAccountIdsBeforeBlockId(uint64_t blockId) const {
  std::vector<uint64_t> ids;
  for (const auto& [id, account] : mAccounts_) {
    if (account.blockId < blockId) {
      ids.push_back(id);
    }
  }
  return ids;
}

bool AccountBuffer::isNegativeBalanceAllowed(const Account& account, uint64_t tokenId) const {
  // Only the genesis token account can have negative balances
  return account.id == tokenId;
}

AccountBuffer::Roe<const AccountBuffer::Account&> AccountBuffer::getAccount(uint64_t id) const {
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
  if (hasAccount(account.id)) {
    return Error(2, "Account already exists");
  }

  mAccounts_[account.id] = account;
  
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::update(const AccountBuffer& other) {
  for (const auto& [id, account] : other.mAccounts_) {
    if (!hasAccount(id)) {
      return Error(8, "Account to update not found: " + std::to_string(id));
    }
    mAccounts_[id] = account;
  }
  return {};
}

std::vector<consensus::Stakeholder> AccountBuffer::getStakeholders() const {
  std::vector<consensus::Stakeholder> stakeholders;
  for (const auto& [id, account] : mAccounts_) {
    auto balanceIt = account.mBalances.find(ID_GENESIS);  
    if (balanceIt == account.mBalances.end()) {
      continue;
    }
    if (balanceIt->second > 0) {
      stakeholders.push_back({id, uint64_t(balanceIt->second)});
    }
  }
  return stakeholders;
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
  
  if (!isNegativeBalanceAllowed(it->second, tokenId) && currentBalance < amount) {
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
  if (!isNegativeBalanceAllowed(fromIt->second, tokenId) && fromBalance < amount) {
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

void AccountBuffer::reset() {
  clear();
}

AccountBuffer::Roe<void> AccountBuffer::addTransaction(uint64_t fromId, uint64_t toId, uint64_t tokenId, int64_t amount, int64_t fee) {
  // Validate amount
  if (amount < 0) {
    return Error(15, "Transfer amount must be non-negative");
  }
  if (amount == 0) {
    return {}; // No-op for zero amount
  }
  
  // Validate fee
  if (fee < 0) {
    return Error(16, "Fee must be non-negative");
  }

  // Check that source account exists
  auto fromIt = mAccounts_.find(fromId);
  if (fromIt == mAccounts_.end()) {
    return Error(17, "Source account not found");
  }

  // Check if fromId has sufficient balance for the transfer amount
  int64_t fromTokenBalance = 0;
  auto fromTokenBalanceIt = fromIt->second.mBalances.find(tokenId);
  if (fromTokenBalanceIt != fromIt->second.mBalances.end()) {
    fromTokenBalance = fromTokenBalanceIt->second;
  }
  
  // Check if fromId has sufficient balance for the fee (in ID_GENESIS token)
  int64_t fromFeeBalance = 0;
  if (tokenId == ID_GENESIS) {
    // If tokenId is ID_GENESIS, the fee comes from the same balance as the transfer
    fromFeeBalance = fromTokenBalance;
  } else {
    auto fromFeeBalanceIt = fromIt->second.mBalances.find(ID_GENESIS);
    if (fromFeeBalanceIt != fromIt->second.mBalances.end()) {
      fromFeeBalance = fromFeeBalanceIt->second;
    }
  }
  
  // Verify sufficient balance (unless negative balance is allowed)
  if (!isNegativeBalanceAllowed(fromIt->second, tokenId)) {
    if (tokenId == ID_GENESIS) {
      // Both amount and fee come from the same balance
      if (fromTokenBalance < amount + fee) {
        return Error(18, "Insufficient balance for transfer and fee");
      }
    } else {
      // Amount and fee come from different balances
      if (fromTokenBalance < amount) {
        return Error(19, "Insufficient balance for transfer");
      }
      if (fromFeeBalance < fee) {
        return Error(20, "Insufficient balance for fee");
      }
    }
  }

  // Create toId account if it doesn't exist
  bool toAccountCreated = false;
  if (!hasAccount(toId)) {
    Account newAccount;
    newAccount.id = toId;
    newAccount.mBalances[tokenId] = 0;
    newAccount.publicKeys = {};
    auto addResult = add(newAccount);
    if (!addResult) {
      return Error(21, "Failed to create destination account: " + addResult.error().message);
    }
    toAccountCreated = true;
  }

  // Perform the transfer
  auto transferResult = transferBalance(fromId, toId, tokenId, amount);
  if (!transferResult) {
    if (toAccountCreated) {
      remove(toId); // Cleanup the newly created account
    }
    return Error(22, "Transfer failed: " + transferResult.error().message);
  }

  // Deduct the fee (if non-zero)
  if (fee > 0) {
    auto feeResult = withdrawBalance(fromId, ID_GENESIS, fee);
    if (!feeResult) {
      // This shouldn't happen since we already checked for sufficient balance,
      // but handle it gracefully anyway
      return Error(23, "Failed to deduct fee: " + feeResult.error().message);
    }
  }

  return {};
}
} // namespace pp
