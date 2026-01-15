#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "Module.h"
#include "Ledger.h"
#include "../consensus/Ouroboros.h"
#include "../interface/Block.hpp"
#include "../ledger/BlockChain.h"
#include "../client/Client.h"
#include "ResultOrError.hpp"
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
#include <set>

// Forward declarations for network classes
namespace pp {
namespace network {
class FetchClient;
class FetchServer;
}
}

namespace pp {

class Server : public Module {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T>
    using Roe = ResultOrError<T, Error>;

    struct NetworkConfig {
        bool enableP2P = false;
        std::string nodeId;
        std::vector<std::string> bootstrapPeers;  // host:port format
        std::string listenAddr = "0.0.0.0";
        uint16_t p2pPort = 9000;
        uint16_t maxPeers = 50;
    };

    Server();
    ~Server();
    
    // Server lifecycle
    bool start(int port);
    bool start(int port, const NetworkConfig& networkConfig);
    void stop();
    bool isRunning() const;
    
    // Network management
    void connectToPeer(const std::string& hostPort);
    size_t getPeerCount() const;
    std::vector<std::string> getConnectedPeers() const;
    bool isP2PEnabled() const;
    
    // Consensus management
    void registerStakeholder(const std::string& id, uint64_t stake);
    void setSlotDuration(uint64_t seconds);
    
    // Transaction management
    void submitTransaction(const std::string& transaction);
    size_t getPendingTransactionCount() const;
    
    // State queries
    uint64_t getCurrentSlot() const;
    uint64_t getCurrentEpoch() const;
    size_t getBlockCount() const;
    Roe<int64_t> getBalance(const std::string& walletId) const;
    
private:
    // Consensus and ledger management
    std::unique_ptr<Ledger> ukpLedger_;
    std::unique_ptr<consensus::Ouroboros> ukpConsensus_;
    
    // Server state
    bool running_;
    int port_;
    std::thread consensusThread_;
    
    // Transaction queue
    mutable std::mutex transactionQueueMutex_;
    std::queue<std::string> transactionQueue_;
    
    // P2P Network components
    std::unique_ptr<network::FetchServer> p2pServer_;
    std::unique_ptr<network::FetchClient> p2pClient_;
    mutable std::mutex peersMutex_;
    std::set<std::string> connectedPeers_;  // Set of host:port strings
    NetworkConfig networkConfig_;
    
    // P2P Network methods
    void initializeP2PNetwork(const NetworkConfig& config);
    void shutdownP2PNetwork();
    std::string handleIncomingRequest(const std::string& request);
    std::string handleClientRequest(const Client::Request& request);
    void broadcastBlock(std::shared_ptr<iii::Block> block);
    void requestBlocksFromPeers(uint64_t fromIndex);
    Roe<std::vector<std::shared_ptr<iii::Block>>> fetchBlocksFromPeer(const std::string& hostPort, uint64_t fromIndex);
    
    // Helper to parse host:port string
    static bool parseHostPort(const std::string& hostPort, std::string& host, uint16_t& port);
    
    // Consensus loop
    void consensusLoop();
    bool shouldProduceBlock() const;
    Roe<void> produceBlock();
    Roe<void> syncState();
    Roe<std::shared_ptr<iii::Block>> createBlockFromTransactions();
    Roe<void> addBlockToLedger(std::shared_ptr<iii::Block> block);
    
    // Chain switching support
    Roe<std::unique_ptr<BlockChain>> buildCandidateChainFromBlocks(
        const std::vector<std::shared_ptr<iii::Block>>& blocks) const;
    Roe<void> switchToChain(std::unique_ptr<BlockChain> candidateChain);
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
