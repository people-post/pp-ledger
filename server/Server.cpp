#include "Server.h"
#include "Logger.h"
#include "FetchClient.h"
#include "FetchServer.h"
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>
#include <sstream>

namespace pp {

Server::Server()
    : Module("server"),
      running_(false),
      port_(0),
      ukpLedger_(std::make_unique<Ledger>()),
      ukpConsensus_(std::make_unique<consensus::Ouroboros>()) {
    auto& logger = logging::getLogger("server");
    logger.info << "Server initialized";
}

Server::~Server() {
    stop();
}

bool Server::start(int port) {
    NetworkConfig defaultConfig;
    defaultConfig.enableP2P = false;
    return start(port, defaultConfig);
}

bool Server::start(int port, const NetworkConfig& networkConfig) {
    if (running_) {
        auto& logger = logging::getLogger("server");
        logger.warning << "Server is already running on port " << port_;
        return false;
    }
    
    port_ = port;
    running_ = true;
    
    networkConfig_ = networkConfig;
    if (networkConfig.enableP2P) {
        try {
            initializeP2PNetwork(networkConfig);
        } catch (const std::exception& e) {
            auto& logger = logging::getLogger("server");
            logger.error << "Failed to initialize P2P network: " << e.what();
            running_ = false;
            return false;
        }
    }
    
    // Set genesis time if not already set
    if (ukpConsensus_->getGenesisTime() == 0) {
        int64_t genesisTime = std::chrono::system_clock::now().time_since_epoch().count();
        ukpConsensus_->setGenesisTime(genesisTime);
    }
    
    // Start consensus thread
    consensusThread_ = std::thread(&Server::consensusLoop, this);
    
    auto& logger = logging::getLogger("server");
    logger.info << "Server started on port " << port_;
    if (networkConfig.enableP2P) {
        logger.info << "P2P networking enabled on " << networkConfig.listenAddr << ":" << networkConfig.p2pPort;
    }
    return true;
}

void Server::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Wait for consensus thread to finish
    if (consensusThread_.joinable()) {
        consensusThread_.join();
    }
    
    if (networkConfig_.enableP2P) {
        shutdownP2PNetwork();
    }
    
    auto& logger = logging::getLogger("server");
    logger.info << "Server stopped";
}

bool Server::isRunning() const {
    return running_;
}

bool Server::parseHostPort(const std::string& hostPort, std::string& host, uint16_t& port) {
    size_t colonPos = hostPort.find_last_of(':');
    if (colonPos == std::string::npos || colonPos == 0 || colonPos == hostPort.length() - 1) {
        return false;
    }
    
    host = hostPort.substr(0, colonPos);
    try {
        int portInt = std::stoi(hostPort.substr(colonPos + 1));
        if (portInt < 0 || portInt > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(portInt);
        return true;
    } catch (...) {
        return false;
    }
}

void Server::connectToPeer(const std::string& hostPort) {
    if (!networkConfig_.enableP2P || !p2pClient_) {
        auto& logger = logging::getLogger("server");
        logger.warning << "Cannot connect to peer: P2P not enabled";
        return;
    }
    
    std::string host;
    uint16_t port;
    if (!parseHostPort(hostPort, host, port)) {
        auto& logger = logging::getLogger("server");
        logger.error << "Invalid host:port format: " << hostPort;
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        connectedPeers_.insert(hostPort);
    }
    
    auto& logger = logging::getLogger("server");
    logger.info << "Added peer: " << hostPort;
}

size_t Server::getPeerCount() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    return connectedPeers_.size();
}

std::vector<std::string> Server::getConnectedPeers() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    return std::vector<std::string>(connectedPeers_.begin(), connectedPeers_.end());
}

bool Server::isP2PEnabled() const {
    return networkConfig_.enableP2P && p2pServer_ != nullptr;
}

void Server::registerStakeholder(const std::string& id, uint64_t stake) {
    ukpConsensus_->registerStakeholder(id, stake);
    auto& logger = logging::getLogger("server");
    logger.info << "Registered stakeholder '" << id << "' with stake: " << stake;
}

void Server::setSlotDuration(uint64_t seconds) {
    ukpConsensus_->setSlotDuration(seconds);
    auto& logger = logging::getLogger("server");
    logger.info << "Slot duration set to " << seconds << "s";
}

void Server::submitTransaction(const std::string& transaction) {
    {
        std::lock_guard<std::mutex> lock(transactionQueueMutex_);
        transactionQueue_.push(transaction);
    }
    ukpLedger_->addTransaction(transaction);
    auto& logger = logging::getLogger("server");
    logger.debug << "Transaction submitted: " << transaction;
}

size_t Server::getPendingTransactionCount() const {
    std::lock_guard<std::mutex> lock(transactionQueueMutex_);
    return transactionQueue_.size();
}

uint64_t Server::getCurrentSlot() const {
    return ukpConsensus_->getCurrentSlot();
}

uint64_t Server::getCurrentEpoch() const {
    return ukpConsensus_->getCurrentEpoch();
}

size_t Server::getBlockCount() const {
    return ukpLedger_->getBlockCount();
}

Server::Roe<int64_t> Server::getBalance(const std::string& walletId) const {
    // Convert from Ledger::Roe to Server::Roe
    auto result = ukpLedger_->getBalance(walletId);
    if (!result) {
        return Error(1, result.error().message);
    }
    return result.value();
}

bool Server::shouldProduceBlock() const {
    // Produce block if we have pending transactions
    if (getPendingTransactionCount() == 0) {
        return false;
    }
    
    // In multi-node setup, check if we are the slot leader
    if (networkConfig_.enableP2P && !networkConfig_.nodeId.empty()) {
        uint64_t currentSlot = ukpConsensus_->getCurrentSlot();
        auto slotLeaderResult = ukpConsensus_->getSlotLeader(currentSlot);
        
        if (slotLeaderResult && slotLeaderResult.value() == networkConfig_.nodeId) {
            return true;
        }
        return false;
    }
    
    // For single-node setup, always produce if there are pending transactions
    return true;
}

Server::Roe<std::shared_ptr<iii::Block>> Server::createBlockFromTransactions() {
    // Check if there are pending transactions
    if (ukpLedger_->getPendingTransactionCount() == 0) {
        return Error(1, "No pending transactions to create block");
    }
    
    // Commit transactions to create a new block
    auto commitResult = ukpLedger_->commitTransactions();
    if (!commitResult) {
        return Error(2, "Failed to commit transactions: " + commitResult.error().message);
    }
    
    // Get the newly created block
    auto latestBlock = ukpLedger_->getLatestBlock();
    if (!latestBlock) {
        return Error(3, "Failed to retrieve newly created block");
    }
    
    // Set slot and slot leader information
    uint64_t currentSlot = ukpConsensus_->getCurrentSlot();
    auto slotLeaderResult = ukpConsensus_->getSlotLeader(currentSlot);
    
    if (!slotLeaderResult) {
        return Error(4, "Failed to get slot leader: " + slotLeaderResult.error().message);
    }
    
    // For now, we'll just store these in the ledger's internal state
    // In a real implementation, we'd need the concrete Block type to set these
    auto& logger = logging::getLogger("server");
    logger.info << "Block created for slot " << currentSlot << " by leader: " << slotLeaderResult.value();
    
    return latestBlock;
}

Server::Roe<void> Server::produceBlock() {
    if (!shouldProduceBlock()) {
        return Server::Roe<void>();
    }
    
    auto blockResult = createBlockFromTransactions();
    if (!blockResult) {
        return Error(blockResult.error().code, blockResult.error().message);
    }
    
    auto addResult = addBlockToLedger(blockResult.value());
    if (!addResult) {
        return Error(addResult.error().code, addResult.error().message);
    }
    
    // Broadcast new block to network peers
    if (networkConfig_.enableP2P && blockResult.value()) {
        broadcastBlock(blockResult.value());
    }
    
    return Server::Roe<void>();
}

Server::Roe<void> Server::addBlockToLedger(std::shared_ptr<iii::Block> block) {
    if (!block) {
        return Error(1, "Block is null");
    }
    
    // Validate block against consensus
    auto validateResult = ukpConsensus_->validateBlock(*block, *ukpLedger_);
    if (!validateResult) {
        return Error(2, "Block validation failed: " + validateResult.error().message);
    }
    
    if (!validateResult.value()) {
        return Error(3, "Block did not pass consensus validation");
    }
    
    auto& logger = logging::getLogger("server");
    logger.info << "Block " << block->getIndex() << " successfully added (slot: " << block->getSlot() << ")";
    
    return Server::Roe<void>();
}

Server::Roe<void> Server::syncState() {
    if (!networkConfig_.enableP2P || connectedPeers_.empty()) {
        return Server::Roe<void>();
    }
    
    auto& logger = logging::getLogger("server");
    
    // Get our current block count
    size_t localBlockCount = ukpLedger_->getBlockCount();
    
    // Request blocks from peers starting from our current height
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers = std::vector<std::string>(connectedPeers_.begin(), connectedPeers_.end());
    }
    
    for (const auto& peerAddr : peers) {
        auto blocksResult = fetchBlocksFromPeer(peerAddr, localBlockCount);
        if (blocksResult && !blocksResult.value().empty()) {
            logger.info << "Fetched " << blocksResult.value().size() << " blocks from peer";
            
            // Validate and add blocks to our ledger
            for (auto& block : blocksResult.value()) {
                // Validate block
                auto validateResult = ukpConsensus_->validateBlock(*block, *ukpLedger_);
                if (validateResult && validateResult.value()) {
                    // Block is valid, would be added to ledger here
                    logger.debug << "Received valid block " << block->getIndex() << " from peer";
                } else {
                    logger.warning << "Received invalid block from peer";
                }
            }
        }
    }
    
    return Server::Roe<void>();
}

void Server::consensusLoop() {
    auto& logger = logging::getLogger("server");
    logger.info << "Consensus loop started";
    
    while (running_) {
        try {
            // Check if we should produce a block
            if (shouldProduceBlock()) {
                auto produceResult = produceBlock();
                if (!produceResult) {
                    logger.warning << "Block production failed: " << produceResult.error().message;
                }
            }
            
            // Periodically sync state with peers
            auto syncResult = syncState();
            if (!syncResult) {
                logger.debug << "State sync error: " << syncResult.error().message;
            }
            
            // Sleep for a short duration before next consensus iteration
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } catch (const std::exception& e) {
            logger.error << "Consensus loop error: " << e.what();
        }
    }
    
    logger.info << "Consensus loop ended";
}

void Server::initializeP2PNetwork(const NetworkConfig& config) {
    auto& logger = logging::getLogger("server");
    logger.info << "Initializing P2P network...";
    
    // Create FetchClient and FetchServer
    p2pClient_ = std::make_unique<network::FetchClient>();
    p2pServer_ = std::make_unique<network::FetchServer>();
    
    // Start the P2P server
    bool started = p2pServer_->start(config.p2pPort, 
        [this](const std::string& request) {
            return handleIncomingRequest(request);
        });
    
    if (!started) {
        throw std::runtime_error("Failed to start P2P server on port " + std::to_string(config.p2pPort));
    }
    
    logger.info << "P2P server started on port " << config.p2pPort;
    
    // Connect to bootstrap peers
    for (const auto& peerAddr : config.bootstrapPeers) {
        connectToPeer(peerAddr);
    }
    
    logger.info << "P2P network initialized with " << config.bootstrapPeers.size() << " bootstrap peers";
}

void Server::shutdownP2PNetwork() {
    auto& logger = logging::getLogger("server");
    logger.info << "Shutting down P2P network...";
            
    if (p2pServer_) {
        p2pServer_->stop();
        p2pServer_.reset();
    }
    
    p2pClient_.reset();
    
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        connectedPeers_.clear();
    }
    
    logger.info << "P2P network shutdown complete";
}

std::string Server::handleIncomingRequest(const std::string& request) {
    auto& logger = logging::getLogger("server");
    logger.debug << "Received network request: " << request;
    
    try {
        auto jsonRequest = nlohmann::json::parse(request);
        std::string requestType = jsonRequest["type"];
        
        if (requestType == "get_blocks") {
            uint64_t fromIndex = jsonRequest["from_index"];
            uint64_t count = jsonRequest.value("count", 10);
            
            nlohmann::json response;
            response["type"] = "blocks";
            response["blocks"] = nlohmann::json::array();
            
            // Get blocks from ledger
            size_t totalBlocks = ukpLedger_->getBlockCount();
            for (uint64_t i = fromIndex; i < std::min(fromIndex + count, totalBlocks); ++i) {
                // In a full implementation, serialize each block
                nlohmann::json blockJson;
                blockJson["index"] = i;
                response["blocks"].push_back(blockJson);
            }
            
            return response.dump();
        } else if (requestType == "new_block") {
            // Receive new block from peer
            logger.info << "Received new block from peer";
            // In full implementation: deserialize and validate block
            return R"({"status":"received"})"; 
        }
    } catch (const std::exception& e) {
        logger.error << "Error handling request: " << e.what();
        return R"({"error":"invalid request"})"; 
    }
    
    return R"({"error":"unknown request type"})"; 
}

void Server::broadcastBlock(std::shared_ptr<iii::Block> block) {
    if (!block || !p2pClient_) {
        return;
    }
    
    auto& logger = logging::getLogger("server");
    logger.info << "Broadcasting block " << block->getIndex() << " to " << connectedPeers_.size() << " peers";
    
    // Create block broadcast message
    nlohmann::json message;
    message["type"] = "new_block";
    message["index"] = block->getIndex();
    message["slot"] = block->getSlot();
    message["prev_hash"] = block->getPreviousHash();
    message["hash"] = block->getHash();
    
    std::string messageStr = message.dump();
    
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers = std::vector<std::string>(connectedPeers_.begin(), connectedPeers_.end());
    }
    
    // Send to each connected peer
    for (const auto& peerAddr : peers) {
        std::string host;
        uint16_t port;
        if (parseHostPort(peerAddr, host, port)) {
            auto result = p2pClient_->fetchSync(host, port, messageStr);
            if (result.isOk()) {
                logger.debug << "Sent block to peer: " << peerAddr;
            } else {
                logger.warning << "Failed to send block to peer " << peerAddr << ": " << result.error().message;
            }
        }
    }
}

void Server::requestBlocksFromPeers(uint64_t fromIndex) {
    auto& logger = logging::getLogger("server");
    logger.info << "Requesting blocks from index " << fromIndex;
    
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers = std::vector<std::string>(connectedPeers_.begin(), connectedPeers_.end());
    }
    
    for (const auto& peerAddr : peers) {
        auto blocksResult = fetchBlocksFromPeer(peerAddr, fromIndex);
        if (blocksResult && !blocksResult.value().empty()) {
            logger.info << "Received " << blocksResult.value().size() << " blocks from peer";
            break; // Got blocks from one peer, that's enough
        }
    }
}

Server::Roe<std::vector<std::shared_ptr<iii::Block>>> 
Server::fetchBlocksFromPeer(const std::string& hostPort, uint64_t fromIndex) {   
    auto& logger = logging::getLogger("server");
    
    if (!p2pClient_) {
        return Error(1, "P2P client not initialized");
    }
    
    std::string host;
    uint16_t port;
    if (!parseHostPort(hostPort, host, port)) {
        return Error(2, "Invalid peer address format");
    }
    
    // Create request
    nlohmann::json request;
    request["type"] = "get_blocks";
    request["from_index"] = fromIndex;
    request["count"] = 100;
    
    try {
        logger.debug << "Fetching blocks from peer: " << hostPort;
        
        auto result = p2pClient_->fetchSync(host, port, request.dump());
        if (!result.isOk()) {
            return Error(3, "Failed to fetch from peer: " + result.error().message);
        }
        
        // In full implementation:
        // 1. Parse the response JSON
        // 2. Deserialize response into Block objects
        
        // Placeholder return
        return std::vector<std::shared_ptr<iii::Block>>();
        
    } catch (const std::exception& e) {
        logger.error << "Error fetching blocks from peer: " << e.what();
        return Error(4, e.what());
    }
}

} // namespace pp
