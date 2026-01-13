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
#include <map>
#include <set>

// libp2p includes
#include <libp2p/host/host.hpp>
#include <libp2p/peer/peer_info.hpp>

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
        std::vector<std::string> bootstrapPeers;  // Multiaddrs of bootstrap peers
        std::string listenAddr = "/ip4/0.0.0.0/tcp/9000";
        uint16_t maxPeers = 50;
    };

    explicit Server(uint32_t blockchainDifficulty = 2);
    ~Server();
    
    // Server lifecycle
    bool start(int port);
    bool start(int port, const NetworkConfig& networkConfig);
    void stop();
    bool isRunning() const;
    
    // Network management
    void connectToPeer(const std::string& multiaddr);
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
    std::shared_ptr<libp2p::Host> p2pHost_;
    mutable std::mutex peersMutex_;
    std::set<std::string> connectedPeers_;  // Set of peer IDs
    NetworkConfig networkConfig_;
    
    // P2P Network methods
    void initializeP2PNetwork(const NetworkConfig& config);
    void shutdownP2PNetwork();
    std::string handleIncomingRequest(const std::string& request);
    void broadcastBlock(std::shared_ptr<iii::Block> block);
    void requestBlocksFromPeers(uint64_t fromIndex);
    Roe<std::vector<std::shared_ptr<iii::Block>>> fetchBlocksFromPeer(const std::string& peerMultiaddr, uint64_t fromIndex);
    
    // Consensus loop
    void consensusLoop();
    bool shouldProduceBlock() const;
    Roe<void> produceBlock();
    Roe<void> syncState();
    Roe<std::shared_ptr<iii::Block>> createBlockFromTransactions();
    Roe<void> addBlockToLedger(std::shared_ptr<iii::Block> block);
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
