#include "Wallet.h"

namespace pp {

Wallet::Wallet() : balance_(0) {}

Wallet::Wallet(int64_t initialBalance) : balance_(initialBalance) {}

int64_t Wallet::getBalance() const { return balance_; }

Wallet::Roe<void> Wallet::deposit(int64_t amount) {
  if (amount < 0) {
    return Wallet::Error(1, "Deposit amount must be non-negative");
  }

  // Check for overflow
  if (balance_ > INT64_MAX - amount) {
    return Wallet::Error(2, "Deposit would cause balance overflow");
  }

  balance_ += amount;
  return {};
}

Wallet::Roe<void> Wallet::withdraw(int64_t amount) {
  if (amount < 0) {
    return Wallet::Error(1, "Withdrawal amount must be non-negative");
  }

  if (balance_ < amount) {
    return Wallet::Error(2, "Insufficient balance");
  }

  balance_ -= amount;
  return {};
}

Wallet::Roe<void> Wallet::transfer(Wallet &destination, int64_t amount) {
  if (amount < 0) {
    return Wallet::Error(1, "Transfer amount must be non-negative");
  }

  if (balance_ < amount) {
    return Wallet::Error(2, "Insufficient balance for transfer");
  }

  // Check destination won't overflow
  if (destination.balance_ > INT64_MAX - amount) {
    return Wallet::Error(3, "Transfer would cause destination overflow");
  }

  balance_ -= amount;
  destination.balance_ += amount;
  return {};
}

bool Wallet::hasBalance(int64_t amount) const { return balance_ >= amount; }

bool Wallet::isEmpty() const { return balance_ == 0; }

void Wallet::reset() { balance_ = 0; }

void Wallet::setBalance(int64_t balance) { balance_ = balance; }

} // namespace pp
