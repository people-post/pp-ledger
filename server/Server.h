#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "Module.h"
#include "Ledger.h"
#include "../consensus/Ouroboros.h"
#include "../interface/Block.hpp"
#include "ResultOrError.hpp"
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>

namespace pp {

class Server : public Module {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T>
    using Roe = ResultOrError<T, Error>;

    explicit Server(uint32_t blockchainDifficulty = 2);
    ~Server();
    
    // Server lifecycle
    bool start(int port);
    void stop();
    bool isRunning() const;
    
    // Consensus management
    void registerStakeholder(const std::string& id, uint64_t stake);
    void setSlotDuration(uint64_t seconds);
    void setGenesisTime(int64_t timestamp);
    
    // Transaction management
    void submitTransaction(const std::string& transaction);
    size_t getPendingTransactionCount() const;
    
    // Block production and synchronization
    Roe<void> produceBlock();
    Roe<void> syncState();
    
    // State queries
    uint64_t getCurrentSlot() const;
    uint64_t getCurrentEpoch() const;
    size_t getBlockCount() const;
    Roe<int64_t> getBalance(const std::string& walletId) const;
    
    // Access to managed objects
    Ledger& getLedger();
    const Ledger& getLedger() const;
    consensus::Ouroboros& getConsensus();
    const consensus::Ouroboros& getConsensus() const;
    
private:
    // Consensus and ledger management
    std::unique_ptr<Ledger> ledger_;
    std::unique_ptr<consensus::Ouroboros> consensus_;
    
    // Server state
    bool running_;
    int port_;
    std::thread consensusThread_;
    
    // Transaction queue
    mutable std::mutex transactionQueueMutex_;
    std::queue<std::string> transactionQueue_;
    
    // Consensus loop
    void consensusLoop();
    bool shouldProduceBlock() const;
    Roe<std::shared_ptr<iii::Block>> createBlockFromTransactions();
    Roe<void> addBlockToLedger(std::shared_ptr<iii::Block> block);
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
