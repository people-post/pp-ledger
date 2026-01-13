#ifndef PP_LEDGER_WALLET_H
#define PP_LEDGER_WALLET_H

#include "ResultOrError.hpp"

#include <string>
#include <cstdint>

namespace pp {

class Wallet {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T>
    using Roe = ResultOrError<T, Error>;
    
    Wallet();
    explicit Wallet(int64_t initialBalance);
    ~Wallet() = default;
    
    // Balance operations
    int64_t getBalance() const;
    Roe<void> deposit(int64_t amount);
    Roe<void> withdraw(int64_t amount);
    Roe<void> transfer(Wallet& destination, int64_t amount);
    
    // Query operations
    bool hasBalance(int64_t amount) const;
    bool isEmpty() const;
    
    // Reset wallet
    void reset();
    void setBalance(int64_t balance);
    
private:
    int64_t balance_;
};

} // namespace pp

#endif // PP_LEDGER_WALLET_H
