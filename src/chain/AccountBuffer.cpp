#include "AccountBuffer.h"

#include "common/Serialize.hpp"
#include "lib/common/Utilities.h"

#include <limits>
#include <sstream>
#include <string>

namespace pp {

AccountBuffer::AccountBuffer() = default;

std::string AccountBuffer::accountLeafHash(const Account &account) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & account.id & account.wallet & account.blockId;
  return utl::sha256Raw(std::string("pp-ledger/account-leaf/v1") + oss.str());
}

void AccountBuffer::touchTree(const Account &account) {
  stateTree_.setLeaf(account.id, accountLeafHash(account));
}

void AccountBuffer::clearTree(uint64_t id) { stateTree_.clearLeaf(id); }

AccountBuffer::Roe<AccountBuffer::Account *>
AccountBuffer::mutableAccount(uint64_t id) {
  auto it = mAccounts_.find(id);
  if (it == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Account not found: " + std::to_string(id));
  }
  return &it->second;
}

bool AccountBuffer::hasAccount(uint64_t id) const {
  return mAccounts_.find(id) != mAccounts_.end();
}

bool AccountBuffer::isEmpty() const { return mAccounts_.empty(); }

bool AccountBuffer::isNegativeBalanceAllowed(const Account &account,
                                             uint64_t tokenId) const {
  // Only the genesis token account can have negative balances
  return account.id < ID_FIRST_USER && account.id == tokenId;
}

std::vector<uint64_t>
AccountBuffer::getAccountIdsBeforeBlockId(uint64_t blockId) const {
  std::vector<uint64_t> ids;
  for (const auto &[id, account] : mAccounts_) {
    if (account.blockId < blockId) {
      ids.push_back(id);
    }
  }
  return ids;
}

AccountBuffer::Roe<const AccountBuffer::Account &>
AccountBuffer::getAccount(uint64_t id) const {
  auto it = mAccounts_.find(id);
  if (it == mAccounts_.end()) {
    return Error(E_ACCOUNT, "Account not found: " + std::to_string(id));
  }
  return it->second;
}

int64_t AccountBuffer::getBalance(uint64_t accountId, uint64_t tokenId) const {
  auto acc = getAccount(accountId);
  if (!acc) {
    return 0;
  }
  auto balanceIt = acc.value().wallet.mBalances.find(tokenId);
  if (balanceIt == acc.value().wallet.mBalances.end()) {
    return 0;
  }
  return balanceIt->second;
}

std::vector<consensus::Stakeholder> AccountBuffer::getStakeholders() const {
  std::vector<consensus::Stakeholder> stakeholders;
  for (const auto &[id, account] : mAccounts_) {
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

AccountBuffer::Roe<void> AccountBuffer::add(const Account &account) {
  if (hasAccount(account.id)) {
    return Error(E_ACCOUNT, "Account already exists");
  }

  mAccounts_[account.id] = account;
  touchTree(account);
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::update(const AccountBuffer &other) {
  for (const auto &[id, account] : other.mAccounts_) {
    if (!hasAccount(id)) {
      return Error(E_ACCOUNT,
                   "Account to update not found: " + std::to_string(id));
    }
    mAccounts_[id] = account;
    touchTree(account);
  }
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::seedFromCommittedIfMissing(
    const AccountBuffer &committed, uint64_t accountId) {
  if (hasAccount(accountId)) {
    return {};
  }
  if (!committed.hasAccount(accountId)) {
    return Error(E_ACCOUNT, "Account not found: " + std::to_string(accountId));
  }

  auto accRoe = committed.getAccount(accountId);
  if (!accRoe) {
    return Error(accRoe.error().code,
                 "Failed to get account: " + accRoe.error().message);
  }

  auto addRoe = add(accRoe.value());
  if (!addRoe) {
    return Error(addRoe.error().code,
                 "Failed to add account to buffer: " + addRoe.error().message);
  }

  return {};
}

AccountBuffer::Roe<void> AccountBuffer::verifySpendingPower(uint64_t accountId,
                                                            uint64_t tokenId,
                                                            uint64_t amount,
                                                            uint64_t fee) const {
  if (amount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Error(E_INPUT, "Amount exceeds int64_t range");
  }
  if (fee > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Error(E_INPUT, "Fee exceeds int64_t range");
  }
  const int64_t amountSigned = static_cast<int64_t>(amount);
  const int64_t feeSigned = static_cast<int64_t>(fee);

  auto accRoe = getAccount(accountId);
  if (!accRoe) {
    return Error(E_ACCOUNT, "Account not found: " + std::to_string(accountId));
  }
  const Account &account = accRoe.value();

  int64_t tokenBalance = 0;
  auto tokenBalanceIt = account.wallet.mBalances.find(tokenId);
  if (tokenBalanceIt != account.wallet.mBalances.end()) {
    tokenBalance = tokenBalanceIt->second;
  }

  int64_t feeBalance = 0;
  if (tokenId == ID_GENESIS) {
    feeBalance = tokenBalance;
  } else {
    auto feeBalanceIt = account.wallet.mBalances.find(ID_GENESIS);
    if (feeBalanceIt != account.wallet.mBalances.end()) {
      feeBalance = feeBalanceIt->second;
    }
  }

  bool allowNegativeTokenBalance = isNegativeBalanceAllowed(account, tokenId);

  if (tokenId == ID_GENESIS) {
    if (allowNegativeTokenBalance) {
      if (amountSigned + feeSigned + INT64_MIN > tokenBalance) {
        return Error(E_BALANCE,
                     "Transfer amount and fee would cause balance underflow");
      }
      return {};
    }
    if (tokenBalance < amountSigned + feeSigned) {
      return Error(E_BALANCE, "Insufficient balance for transfer and fee");
    }
  } else {
    if (allowNegativeTokenBalance) {
      if (amountSigned + INT64_MIN > tokenBalance) {
        return Error(E_BALANCE,
                     "Transfer amount would cause balance underflow");
      }
    } else if (tokenBalance < amountSigned) {
      return Error(E_BALANCE, "Insufficient balance for transfer");
    }
    if (feeBalance < feeSigned) {
      return Error(E_BALANCE, "Insufficient balance for fee");
    }
  }

  return {};
}

AccountBuffer::Roe<void> AccountBuffer::verifyBalance(
    uint64_t accountId, uint64_t amount, uint64_t fee,
    const std::map<uint64_t, int64_t> &expectedBalances) const {
  if (amount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Error(E_INPUT, "Amount exceeds int64_t range");
  }
  if (fee > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Error(E_INPUT, "Fee exceeds int64_t range");
  }
  const int64_t amountSigned = static_cast<int64_t>(amount);
  const int64_t feeSigned = static_cast<int64_t>(fee);

  auto accRoe = getAccount(accountId);
  if (!accRoe) {
    return Error(E_ACCOUNT, "Account not found: " + std::to_string(accountId));
  }

  const auto &account = accRoe.value();
  const auto &bufferBalances = account.wallet.mBalances;

  auto getBalanceOrZero = [](const std::map<uint64_t, int64_t> &balances,
                             uint64_t tokenId) -> int64_t {
    auto balanceIt = balances.find(tokenId);
    if (balanceIt == balances.end()) {
      return 0;
    }
    return balanceIt->second;
  };

  auto safeAdd = [](int64_t a, int64_t b, int64_t &out) -> bool {
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<int64_t>::min() - b)) {
      return false;
    }
    out = a + b;
    return true;
  };

  for (const auto &[tokenId, bufferBalance] : bufferBalances) {
    if (tokenId == ID_GENESIS) {
      continue;
    }
    int64_t expectedBalance = getBalanceOrZero(expectedBalances, tokenId);
    if (bufferBalance != expectedBalance) {
      return Error(E_BALANCE,
                   "Balance mismatch for token " + std::to_string(tokenId));
    }
  }

  for (const auto &[tokenId, expectedBalance] : expectedBalances) {
    if (tokenId == ID_GENESIS) {
      continue;
    }
    int64_t bufferBalance = getBalanceOrZero(bufferBalances, tokenId);
    if (bufferBalance != expectedBalance) {
      return Error(E_BALANCE,
                   "Balance mismatch for token " + std::to_string(tokenId));
    }
  }

  int64_t delta = 0;
  if (!safeAdd(amountSigned, feeSigned, delta)) {
    return Error(E_BALANCE, "Amount and fee overflow");
  }

  int64_t expectedGenesis = getBalanceOrZero(expectedBalances, ID_GENESIS);
  int64_t expectedBufferGenesis = 0;
  if (!safeAdd(expectedGenesis, delta, expectedBufferGenesis)) {
    return Error(E_BALANCE,
                 "Genesis token balance overflow when adding amount and fee");
  }

  int64_t bufferGenesis = getBalanceOrZero(bufferBalances, ID_GENESIS);
  if (bufferGenesis != expectedBufferGenesis) {
    return Error(E_BALANCE,
                 "Genesis token balance mismatch for account " +
                     std::to_string(accountId) + ": expected " +
                     std::to_string(expectedBufferGenesis) + ", got " +
                     std::to_string(bufferGenesis));
  }

  return {};
}

AccountBuffer::Roe<void> AccountBuffer::depositBalance(uint64_t accountId,
                                                       uint64_t tokenId,
                                                       int64_t amount) {
  if (amount < 0) {
    return Error(E_INPUT, "Deposit amount must be non-negative");
  }

  auto accRoe = mutableAccount(accountId);
  if (!accRoe) {
    return Error(accRoe.error().code, accRoe.error().message);
  }
  Account &account = *accRoe.value();

  int64_t currentBalance = 0;
  auto balanceIt = account.wallet.mBalances.find(tokenId);
  if (balanceIt != account.wallet.mBalances.end()) {
    currentBalance = balanceIt->second;
  }

  if (currentBalance > INT64_MAX - amount) {
    return Error(E_BALANCE, "Deposit would cause balance overflow");
  }
  account.wallet.mBalances[tokenId] = currentBalance + amount;
  touchTree(account);
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::withdrawBalance(uint64_t accountId,
                                                        uint64_t tokenId,
                                                        int64_t amount) {
  if (amount < 0) {
    return Error(E_INPUT, "Withdraw amount must be non-negative");
  }

  auto accRoe = mutableAccount(accountId);
  if (!accRoe) {
    return Error(accRoe.error().code, accRoe.error().message);
  }
  Account &account = *accRoe.value();

  int64_t currentBalance = 0;
  auto balanceIt = account.wallet.mBalances.find(tokenId);
  if (balanceIt != account.wallet.mBalances.end()) {
    currentBalance = balanceIt->second;
  }

  if (!isNegativeBalanceAllowed(account, tokenId) && currentBalance < amount) {
    return Error(E_BALANCE, "Insufficient balance");
  }
  if (currentBalance < INT64_MIN + amount) {
    return Error(E_BALANCE, "Withdraw would cause balance underflow");
  }
  account.wallet.mBalances[tokenId] = currentBalance - amount;
  touchTree(account);
  return {};
}

AccountBuffer::Roe<void>
AccountBuffer::transferBalance(uint64_t fromId, uint64_t toId, uint64_t tokenId,
                               uint64_t amount, uint64_t fee) {
  auto spendingResult = verifySpendingPower(fromId, tokenId, amount, fee);
  if (!spendingResult) {
    return spendingResult;
  }
  const int64_t amountSigned = static_cast<int64_t>(amount);
  const int64_t feeSigned = static_cast<int64_t>(fee);

  if (auto fromRoe = mutableAccount(fromId); !fromRoe) {
    return Error(E_ACCOUNT,
                 "Source account not found: " + std::to_string(fromId));
  }
  if (auto toRoe = mutableAccount(toId); !toRoe) {
    return Error(E_ACCOUNT,
                 "Destination account not found: " + std::to_string(toId));
  }
  if (fee > 0) {
    if (auto feeRoe = mutableAccount(ID_FEE); !feeRoe) {
      return Error(E_ACCOUNT,
                   "Fee account not found: " + std::to_string(ID_FEE));
    }
  }

  Account &fromAcc = mAccounts_.at(fromId);
  Account &toAcc = mAccounts_.at(toId);

  int64_t fromBalance = 0;
  auto fromBalanceIt = fromAcc.wallet.mBalances.find(tokenId);
  if (fromBalanceIt != fromAcc.wallet.mBalances.end()) {
    fromBalance = fromBalanceIt->second;
  }

  int64_t toBalance = 0;
  auto toBalanceIt = toAcc.wallet.mBalances.find(tokenId);
  if (toBalanceIt != toAcc.wallet.mBalances.end()) {
    toBalance = toBalanceIt->second;
  }

  if (toBalance > INT64_MAX - amountSigned) {
    return Error(E_INPUT, "Transfer would cause balance overflow");
  }

  int64_t genesisBalance = 0;
  if (fee > 0 && tokenId != ID_GENESIS) {
    auto genesisBalanceIt = fromAcc.wallet.mBalances.find(ID_GENESIS);
    if (genesisBalanceIt != fromAcc.wallet.mBalances.end()) {
      genesisBalance = genesisBalanceIt->second;
    }
  }

  fromAcc.wallet.mBalances[tokenId] = fromBalance - amountSigned;
  toAcc.wallet.mBalances[tokenId] = toBalance + amountSigned;

  if (fee > 0) {
    if (tokenId == ID_GENESIS) {
      fromAcc.wallet.mBalances[ID_GENESIS] =
          fromBalance - amountSigned - feeSigned;
    } else {
      fromAcc.wallet.mBalances[ID_GENESIS] = genesisBalance - feeSigned;
    }

    Account &feeAcc = mAccounts_.at(ID_FEE);
    int64_t feeAccountBalance = 0;
    auto feeBalanceIt = feeAcc.wallet.mBalances.find(ID_GENESIS);
    if (feeBalanceIt != feeAcc.wallet.mBalances.end()) {
      feeAccountBalance = feeBalanceIt->second;
    }
    if (feeAccountBalance > INT64_MAX - feeSigned) {
      return Error(E_INPUT, "Fee account balance would overflow");
    }
    feeAcc.wallet.mBalances[ID_GENESIS] = feeAccountBalance + feeSigned;
    touchTree(feeAcc);
  }

  touchTree(fromAcc);
  touchTree(toAcc);
  return {};
}

AccountBuffer::Roe<void> AccountBuffer::writeOff(uint64_t accountId) {
  auto accountRoe = mutableAccount(accountId);
  if (!accountRoe) {
    return Error(E_ACCOUNT, "Account not found: " + std::to_string(accountId));
  }
  if (auto recycleRoe = mutableAccount(ID_RECYCLE); !recycleRoe) {
    return Error(E_ACCOUNT,
                 "Recycle account not found: " + std::to_string(ID_RECYCLE));
  }

  const std::map<uint64_t, int64_t> balances =
      mAccounts_.at(accountId).wallet.mBalances;
  Account &recycle = mAccounts_.at(ID_RECYCLE);

  for (const auto &[tokenId, amount] : balances) {
    if (amount > 0) {
      recycle.wallet.mBalances[tokenId] += amount;
    }
  }

  touchTree(recycle);
  remove(accountId);
  return {};
}

void AccountBuffer::remove(uint64_t id) {
  if (!hasAccount(id)) {
    return;
  }
  mAccounts_.erase(id);
  clearTree(id);
}

void AccountBuffer::clear() {
  mAccounts_.clear();
  stateTree_ = AccountStateTree{};
}

void AccountBuffer::reset() { clear(); }

std::string AccountBuffer::calculateStateRoot() const {
  return stateTree_.root();
}

} // namespace pp
