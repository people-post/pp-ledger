#include "AccountBuffer.h"

namespace pp {

AccountBuffer::AccountBuffer() {
}

bool AccountBuffer::hasAccount(uint64_t id) const {
  return mAccounts_.find(id) != mAccounts_.end();
}

bool AccountBuffer::isEmpty() const {
  return mAccounts_.empty();
}

bool AccountBuffer::isNegativeBalanceAllowed(const Account& account, uint64_t tokenId) const {
  // Only the genesis token account can have negative balances
  return account.id == tokenId;
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

AccountBuffer::Roe<const AccountBuffer::Account&> AccountBuffer::getAccount(uint64_t id) const {
  auto it = mAccounts_.find(id);
  if (it == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Account not found");
  }
  return it->second;
}

int64_t AccountBuffer::getBalance(uint64_t accountId, uint64_t tokenId) const {
  auto it = mAccounts_.find(accountId);
  if (it == mAccounts_.end()) {
    return 0;
  }
  auto balanceIt = it->second.wallet.mBalances.find(tokenId);
  if (balanceIt == it->second.wallet.mBalances.end()) {
    return 0;
  }
  return balanceIt->second;
}

std::vector<consensus::Stakeholder> AccountBuffer::getStakeholders() const {
  std::vector<consensus::Stakeholder> stakeholders;
  for (const auto& [id, account] : mAccounts_) {
    auto balanceIt = account.wallet.mBalances.find(ID_GENESIS);  
    if (balanceIt == account.wallet.mBalances.end()) {
      continue;
    }
    if (balanceIt->second > 0) {
      stakeholders.push_back({id, uint64_t(balanceIt->second)});
    }
  }
  return stakeholders;
}

AccountBuffer::Roe<void> AccountBuffer::add(const Account& account) {
  if (hasAccount(account.id)) {
    return Error(E_ACCOUNT, "Account already exists");
  }

  mAccounts_[account.id] = account;
  
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::update(const AccountBuffer& other) {
  for (const auto& [id, account] : other.mAccounts_) {
    if (!hasAccount(id)) {
      return Error(E_ACCOUNT, "Account to update not found: " + std::to_string(id));
    }
    mAccounts_[id] = account;
  }
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::verifySpendingPower(uint64_t accountId, uint64_t tokenId, int64_t amount, int64_t fee) const {
  // Validate inputs
  if (amount < 0) {
    return Error(E_INPUT, "Transfer amount must be non-negative");
  }
  if (fee < 0) {
    return Error(E_INPUT, "Fee must be non-negative");
  }
  
  // Check if account exists
  auto it = mAccounts_.find(accountId);
  if (it == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Account not found");
  }
  
  // Get token balance
  int64_t tokenBalance = 0;
  auto tokenBalanceIt = it->second.wallet.mBalances.find(tokenId);
  if (tokenBalanceIt != it->second.wallet.mBalances.end()) {
    tokenBalance = tokenBalanceIt->second;
  }
  
  // Get fee balance (in ID_GENESIS token)
  int64_t feeBalance = 0;
  if (tokenId == ID_GENESIS) {
    // If tokenId is ID_GENESIS, the fee comes from the same balance as the transfer
    feeBalance = tokenBalance;
  } else {
    auto feeBalanceIt = it->second.wallet.mBalances.find(ID_GENESIS);
    if (feeBalanceIt != it->second.wallet.mBalances.end()) {
      feeBalance = feeBalanceIt->second;
    }
  }
  
  // Check if negative balance is allowed for this token (only for genesis token account)
  bool allowNegativeTokenBalance = isNegativeBalanceAllowed(it->second, tokenId);
  
  // Check sufficient balance
  if (tokenId == ID_GENESIS) {
    // Both amount and fee come from the same balance
    // For genesis account, allow negative balance
    if (allowNegativeTokenBalance) {
      if (amount + fee + INT64_MIN > tokenBalance) {
        return Error(E_BALANCE, "Transfer amount and fee would cause balance underflow");
      }
      return {};
    }
    if (tokenBalance < amount + fee) {
      return Error(E_BALANCE, "Insufficient balance for transfer and fee");
    }
  } else {
    // Amount and fee come from different balances
    // For custom token genesis account: can have negative token balance, but must have enough fee in ID_GENESIS
    if (allowNegativeTokenBalance) {
      // For custom token genesis account, check for underflow
      if (amount + INT64_MIN > tokenBalance) {
        return Error(E_BALANCE, "Transfer amount would cause balance underflow");
      }
    } else if (tokenBalance < amount) {
      return Error(E_BALANCE, "Insufficient balance for transfer");
    }
    if (feeBalance < fee) {
      return Error(E_BALANCE, "Insufficient balance for fee");
    }
  }
  
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::depositBalance(uint64_t accountId, uint64_t tokenId, int64_t amount) {
  if (amount < 0) {
    return Error(E_INPUT, "Deposit amount must be non-negative");
  }

  auto it = mAccounts_.find(accountId);
  if (it == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Account not found");
  }
  
  int64_t currentBalance = 0;
  auto balanceIt = it->second.wallet.mBalances.find(tokenId);
  if (balanceIt != it->second.wallet.mBalances.end()) {
    currentBalance = balanceIt->second;
  }
  
  if (currentBalance > INT64_MAX - amount) {
    return Error(E_BALANCE, "Deposit would cause balance overflow");
  }
  it->second.wallet.mBalances[tokenId] = currentBalance + amount;
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::withdrawBalance(uint64_t accountId, uint64_t tokenId, int64_t amount) {
  if (amount < 0) {
    return Error(E_INPUT, "Withdraw amount must be non-negative");
  }

  auto it = mAccounts_.find(accountId);
  if (it == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Account not found");
  }
  
  int64_t currentBalance = 0;
  auto balanceIt = it->second.wallet.mBalances.find(tokenId);
  if (balanceIt != it->second.wallet.mBalances.end()) {
    currentBalance = balanceIt->second;
  }
  
  if (!isNegativeBalanceAllowed(it->second, tokenId) && currentBalance < amount) {
    return Error(E_BALANCE, "Insufficient balance");
  }
  if (currentBalance < INT64_MIN + amount) {
    return Error(E_BALANCE, "Withdraw would cause balance underflow");
  }
  it->second.wallet.mBalances[tokenId] = currentBalance - amount;
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::transferBalance(uint64_t fromId, uint64_t toId, uint64_t tokenId, int64_t amount, int64_t fee) {
  // Verify spending power of source account
  auto spendingResult = verifySpendingPower(fromId, tokenId, amount, fee);
  if (!spendingResult) {
    return spendingResult;
  }

  auto fromIt = mAccounts_.find(fromId);
  if (fromIt == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Source account not found");
  }

  auto toIt = mAccounts_.find(toId);
  if (toIt == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Destination account not found");
  }

  int64_t fromBalance = 0;
  auto fromBalanceIt = fromIt->second.wallet.mBalances.find(tokenId);
  if (fromBalanceIt != fromIt->second.wallet.mBalances.end()) {
    fromBalance = fromBalanceIt->second;
  }
  
  int64_t toBalance = 0;
  auto toBalanceIt = toIt->second.wallet.mBalances.find(tokenId);
  if (toBalanceIt != toIt->second.wallet.mBalances.end()) {
    toBalance = toBalanceIt->second;
  }

  // Check for overflow in destination account
  if (toBalance > INT64_MAX - amount) {
    return Error(E_INPUT, "Transfer would cause balance overflow");
  }

  // Get fee balance if needed
  int64_t genesisBalance = 0;
  if (fee > 0 && tokenId != ID_GENESIS) {
    // For custom token transfers, retrieve ID_GENESIS balance for fee deduction
    auto genesisBalanceIt = fromIt->second.wallet.mBalances.find(ID_GENESIS);
    if (genesisBalanceIt != fromIt->second.wallet.mBalances.end()) {
      genesisBalance = genesisBalanceIt->second;
    }
  }

  // Perform the transfer
  fromIt->second.wallet.mBalances[tokenId] = fromBalance - amount;
  toIt->second.wallet.mBalances[tokenId] = toBalance + amount;

  // Deduct the fee if non-zero
  if (fee > 0) {
    if (tokenId == ID_GENESIS) {
      // Fee already accounted for in the balance check above
      fromIt->second.wallet.mBalances[ID_GENESIS] = fromBalance - amount - fee;
    } else {
      // Deduct fee from ID_GENESIS balance (already retrieved above)
      fromIt->second.wallet.mBalances[ID_GENESIS] = genesisBalance - fee;
    }
  }

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

} // namespace pp
