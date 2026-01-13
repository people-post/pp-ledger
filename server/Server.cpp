#include "Server.h"
#include "Logger.h"
#include <chrono>
#include <thread>

namespace pp {

Server::Server(uint32_t blockchainDifficulty)
    : Module("server"),
      running_(false),
      port_(0),
      ledger_(std::make_unique<Ledger>(blockchainDifficulty)),
      consensus_(std::make_unique<consensus::Ouroboros>()) {
    auto& logger = logging::getLogger("server");
    logger.info << "Server initialized with difficulty: " << blockchainDifficulty;
}

Server::~Server() {
    stop();
}

bool Server::start(int port) {
    if (running_) {
        auto& logger = logging::getLogger("server");
        logger.warning << "Server is already running on port " << port_;
        return false;
    }
    
    port_ = port;
    running_ = true;
    
    // Set genesis time if not already set
    if (consensus_->getGenesisTime() == 0) {
        int64_t genesisTime = std::chrono::system_clock::now().time_since_epoch().count();
        consensus_->setGenesisTime(genesisTime);
    }
    
    // Start consensus thread
    consensusThread_ = std::thread(&Server::consensusLoop, this);
    
    auto& logger = logging::getLogger("server");
    logger.info << "Server started on port " << port_;
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
    
    auto& logger = logging::getLogger("server");
    logger.info << "Server stopped";
}

bool Server::isRunning() const {
    return running_;
}

void Server::registerStakeholder(const std::string& id, uint64_t stake) {
    consensus_->registerStakeholder(id, stake);
    auto& logger = logging::getLogger("server");
    logger.info << "Registered stakeholder '" << id << "' with stake: " << stake;
}

void Server::setSlotDuration(uint64_t seconds) {
    consensus_->setSlotDuration(seconds);
    auto& logger = logging::getLogger("server");
    logger.info << "Slot duration set to " << seconds << "s";
}

void Server::setGenesisTime(int64_t timestamp) {
    consensus_->setGenesisTime(timestamp);
}

void Server::submitTransaction(const std::string& transaction) {
    {
        std::lock_guard<std::mutex> lock(transactionQueueMutex_);
        transactionQueue_.push(transaction);
    }
    ledger_->addTransaction(transaction);
    auto& logger = logging::getLogger("server");
    logger.debug << "Transaction submitted: " << transaction;
}

size_t Server::getPendingTransactionCount() const {
    std::lock_guard<std::mutex> lock(transactionQueueMutex_);
    return transactionQueue_.size();
}

uint64_t Server::getCurrentSlot() const {
    return consensus_->getCurrentSlot();
}

uint64_t Server::getCurrentEpoch() const {
    return consensus_->getCurrentEpoch();
}

size_t Server::getBlockCount() const {
    return ledger_->getBlockCount();
}

Server::Roe<int64_t> Server::getBalance(const std::string& walletId) const {
    // Convert from Ledger::Roe to Server::Roe
    auto result = ledger_->getBalance(walletId);
    if (!result) {
        return Error(1, result.error().message);
    }
    return result.value();
}

Ledger& Server::getLedger() {
    return *ledger_;
}

const Ledger& Server::getLedger() const {
    return *ledger_;
}

consensus::Ouroboros& Server::getConsensus() {
    return *consensus_;
}

const consensus::Ouroboros& Server::getConsensus() const {
    return *consensus_;
}

bool Server::shouldProduceBlock() const {
    // Produce block if we have pending transactions
    if (getPendingTransactionCount() == 0) {
        return false;
    }
    
    // TODO: Implement multi-node support to check if this node is the slot leader
    // For now, always produce if there are pending transactions
    return true;
}

Server::Roe<std::shared_ptr<iii::Block>> Server::createBlockFromTransactions() {
    // Get pending transactions from ledger
    const auto& pendingTransactions = ledger_->getPendingTransactions();
    
    if (pendingTransactions.empty()) {
        return Error(1, "No pending transactions to create block");
    }
    
    // Commit transactions to create a new block
    auto commitResult = ledger_->commitTransactions();
    if (!commitResult) {
        return Error(2, "Failed to commit transactions: " + commitResult.error().message);
    }
    
    // Get the newly created block
    auto latestBlock = ledger_->getBlockChain().getLatestBlock();
    if (!latestBlock) {
        return Error(3, "Failed to retrieve newly created block");
    }
    
    // Set slot and slot leader information
    uint64_t currentSlot = consensus_->getCurrentSlot();
    auto slotLeaderResult = consensus_->getSlotLeader(currentSlot);
    
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
    
    return Server::Roe<void>();
}

Server::Roe<void> Server::addBlockToLedger(std::shared_ptr<iii::Block> block) {
    if (!block) {
        return Error(1, "Block is null");
    }
    
    // Validate block against consensus
    auto validateResult = consensus_->validateBlock(*block, ledger_->getBlockChain());
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
    // In a multi-node setup, this would:
    // 1. Request state from peer nodes
    // 2. Validate received blocks
    // 3. Update local blockchain if peer chain is better
    // 4. Replay transactions to sync ledger state
    
    // For now, this is a placeholder for multi-node synchronization
    auto& logger = logging::getLogger("server");
    logger.debug << "State sync requested (multi-node support not yet implemented)";
    
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

} // namespace pp

