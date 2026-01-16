#include "Server.h"
#include "../client/Client.h"
#include "../ledger/Block.h"
#include "BinaryPack.hpp"
#include "Logger.h"
#include "Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace pp {

Server::Server() : Service("server") { log().info << "Server initialized"; }

bool Server::start(const std::string &dataDir) {
  if (isRunning()) {
    log().warning << "Server is already running on port " << port_;
    return false;
  }

  // Load configuration from config.json
  std::filesystem::path configPath =
      std::filesystem::path(dataDir) / "config.json";
  auto configResult = loadConfig(configPath.string());
  if (!configResult) {
    log().error << "Failed to load configuration: "
                << configResult.error().message;
    return false;
  }

  // Initialize storage first
  auto storageResult = initStorage(dataDir);
  if (!storageResult) {
    log().error << "Failed to initialize storage: "
                << storageResult.error().message;
    return false;
  }

  // Set genesis time if not already set
  if (consensus_.getGenesisTime() == 0) {
    int64_t genesisTime =
        std::chrono::system_clock::now().time_since_epoch().count();
    consensus_.setGenesisTime(genesisTime);
  }

  // Call base class start() which will call onStart() then spawn thread
  return Service::start();
}

bool Server::onStart() {
  // Start client server (listens on main port for client connections)
  bool clientServerStarted =
      sFetch_.start(port_, [this](const std::string &request) {
        return handleIncomingRequest(request);
      });

  if (!clientServerStarted) {
    log().error << "Failed to start client server on port " << port_;
    return false;
  }

  log().info << "Server started on port " << port_;
  return true;
}

void Server::onStop() {
  // Stop client server
  sFetch_.stop();
}

bool Server::parseHostPort(const std::string &hostPort, std::string &host,
                           uint16_t &port) {
  return utl::parseHostPort(hostPort, host, port);
}

size_t Server::getPeerCount() const {
  std::lock_guard<std::mutex> lock(peersMutex_);
  return connectedPeers_.size();
}

std::vector<std::string> Server::getConnectedPeers() const {
  std::lock_guard<std::mutex> lock(peersMutex_);
  return std::vector<std::string>(connectedPeers_.begin(),
                                  connectedPeers_.end());
}

void Server::registerStakeholder(const std::string &id, uint64_t stake) {
  consensus_.registerStakeholder(id, stake);
  auto &logger = logging::getLogger("server");
  logger.info << "Registered stakeholder '" << id << "' with stake: " << stake;
}

void Server::setSlotDuration(uint64_t seconds) {
  consensus_.setSlotDuration(seconds);
  auto &logger = logging::getLogger("server");
  logger.info << "Slot duration set to " << seconds << "s";
}

void Server::submitTransaction(const std::string &transaction) {
  auto &logger = logging::getLogger("server");

  // Deserialize transaction string to Transaction struct using utility function
  auto txResult = utl::binaryUnpack<Ledger::Transaction>(transaction);
  if (!txResult) {
    logger.error << "Failed to deserialize transaction: "
                 << txResult.error().message;
    return;
  }

  auto result = ledger_.addTransaction(txResult.value());
  if (!result) {
    logger.error << "Failed to add transaction: " << result.error().message;
    return;
  }

  logger.debug << "Transaction submitted (fromWallet: "
               << txResult.value().fromWallet
               << ", toWallet: " << txResult.value().toWallet
               << ", amount: " << txResult.value().amount << ")";
}

uint64_t Server::getCurrentSlot() const { return consensus_.getCurrentSlot(); }

uint64_t Server::getCurrentEpoch() const {
  return consensus_.getCurrentEpoch();
}

size_t Server::getBlockCount() const { return ledger_.getBlockCount(); }

Server::Roe<int64_t> Server::getBalance(const std::string &walletId) const {
  // Convert from Ledger::Roe to Server::Roe
  auto result = ledger_.getBalance(walletId);
  if (!result) {
    return Error(1, result.error().message);
  }
  return result.value();
}

bool Server::shouldProduceBlock() const {
  // Produce block if we have pending transactions
  if (ledger_.getPendingTransactionCount() == 0) {
    return false;
  }

  // In multi-node setup, check if we are the slot leader
  if (!networkConfig_.nodeId.empty()) {
    uint64_t currentSlot = consensus_.getCurrentSlot();
    auto slotLeaderResult = consensus_.getSlotLeader(currentSlot);

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
  if (ledger_.getPendingTransactionCount() == 0) {
    return Error(1, "No pending transactions to create block");
  }

  // Commit transactions to create a new block
  auto commitResult = ledger_.commitTransactions();
  if (!commitResult) {
    return Error(2, "Failed to commit transactions: " +
                        commitResult.error().message);
  }

  // Get the newly created block
  auto latestBlock = ledger_.getLatestBlock();
  if (!latestBlock) {
    return Error(3, "Failed to retrieve newly created block");
  }

  // Set slot and slot leader information
  uint64_t currentSlot = consensus_.getCurrentSlot();
  auto slotLeaderResult = consensus_.getSlotLeader(currentSlot);

  if (!slotLeaderResult) {
    return Error(4, "Failed to get slot leader: " +
                        slotLeaderResult.error().message);
  }

  // For now, we'll just store these in the ledger's internal state
  // In a real implementation, we'd need the concrete Block type to set these
  auto &logger = logging::getLogger("server");
  logger.info << "Block created for slot " << currentSlot
              << " by leader: " << slotLeaderResult.value();

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
  if (blockResult.value()) {
    broadcastBlock(blockResult.value());
  }

  return Server::Roe<void>();
}

Server::Roe<void> Server::addBlockToLedger(std::shared_ptr<iii::Block> block) {
  if (!block) {
    return Error(1, "Block is null");
  }

  // Validate block against consensus
  auto validateResult = consensus_.validateBlock(*block, ledger_);
  if (!validateResult) {
    return Error(2,
                 "Block validation failed: " + validateResult.error().message);
  }

  if (!validateResult.value()) {
    return Error(3, "Block did not pass consensus validation");
  }

  auto &logger = logging::getLogger("server");
  logger.info << "Block " << block->getIndex()
              << " successfully added (slot: " << block->getSlot() << ")";

  return Server::Roe<void>();
}

Server::Roe<void> Server::syncState() {
  if (connectedPeers_.empty()) {
    return Server::Roe<void>();
  }

  auto &logger = logging::getLogger("server");

  // Get our current block count
  size_t localBlockCount = ledger_.getBlockCount();

  // Request blocks from peers starting from our current height
  std::vector<std::string> peers;
  {
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers = std::vector<std::string>(connectedPeers_.begin(),
                                     connectedPeers_.end());
  }

  for (const auto &peerAddr : peers) {
    auto blocksResult = fetchBlocksFromPeer(peerAddr, localBlockCount);
    if (blocksResult && !blocksResult.value().empty()) {
      logger.info << "Fetched " << blocksResult.value().size()
                  << " blocks from peer";

      // Build candidate chain from received blocks
      auto candidateChainResult =
          buildCandidateChainFromBlocks(blocksResult.value());
      if (!candidateChainResult) {
        logger.warning << "Failed to build candidate chain: "
                       << candidateChainResult.error().message;
        continue;
      }

      auto candidateChain = std::move(candidateChainResult.value());

      // Validate all blocks in candidate chain against consensus
      bool allBlocksValid = true;
      for (size_t i = 0; i < candidateChain->getSize(); ++i) {
        auto block = candidateChain->getBlock(i);
        if (block) {
          auto validateResult = consensus_.validateBlock(*block, ledger_);
          if (!validateResult || !validateResult.value()) {
            logger.warning << "Block " << block->getIndex()
                           << " in candidate chain failed validation";
            allBlocksValid = false;
            break;
          }
        }
      }

      if (!allBlocksValid) {
        logger.warning << "Candidate chain contains invalid blocks, skipping";
        continue;
      }

      // Check if we should switch to this candidate chain
      auto switchResult =
          consensus_.shouldSwitchChain(ledger_, *candidateChain);
      if (!switchResult) {
        logger.warning << "Chain switch check failed: "
                       << switchResult.error().message;
        continue;
      }

      if (switchResult.value()) {
        logger.info << "Candidate chain is longer and valid, switching chains";
        auto switchChainResult = switchToChain(std::move(candidateChain));
        if (!switchChainResult) {
          logger.error << "Failed to switch chains: "
                       << switchChainResult.error().message;
        } else {
          logger.info << "Successfully switched to longer chain";
          // Break after successful switch to avoid processing more peers
          break;
        }
      } else {
        logger.debug << "Current chain is longer or equal, not switching";
      }
    }
  }

  return Server::Roe<void>();
}

void Server::run() {
  log().info << "Consensus loop started";

  while (isRunning()) {
    try {
      // Check if we should produce a block
      if (shouldProduceBlock()) {
        auto produceResult = produceBlock();
        if (!produceResult) {
          log().warning << "Block production failed: "
                        << produceResult.error().message;
        }
      }

      // Periodically sync state with peers
      auto syncResult = syncState();
      if (!syncResult) {
        log().debug << "State sync error: " << syncResult.error().message;
      }

      // Sleep for a short duration before next consensus iteration
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } catch (const std::exception &e) {
      log().error << "Consensus loop error: " << e.what();
    }
  }

  log().info << "Consensus loop ended";
}

Server::Roe<void> Server::loadConfig(const std::string &configPath) {
  if (!std::filesystem::exists(configPath)) {
    return Error(1, "Configuration file not found: " + configPath);
  }

  std::ifstream configFile(configPath);
  if (!configFile.is_open()) {
    return Error(2, "Failed to open configuration file: " + configPath);
  }

  // Read file content
  std::string content((std::istreambuf_iterator<char>(configFile)),
                      std::istreambuf_iterator<char>());
  configFile.close();

  // Parse JSON with explicit error handling
  nlohmann::json config;
  try {
    config = nlohmann::json::parse(content);
  } catch (const nlohmann::json::parse_error &e) {
    return Error(4, "Failed to parse JSON: " + std::string(e.what()));
  }

  // Load port
  if (!config.contains("port") || !config["port"].is_number()) {
    return Error(3, "Configuration file missing or invalid 'port' field");
  }
  
  // Use value() with default to avoid exception
  if (config["port"].is_number_integer()) {
    port_ = config["port"].get<int>();
  } else {
    return Error(3, "Configuration file 'port' field is not an integer");
  }

  // Load network configuration (optional, defaults will be used if not
  // present)
  if (config.contains("network") && config["network"].is_object()) {
    const auto &network = config["network"];
    if (network.contains("nodeId") && network["nodeId"].is_string()) {
      networkConfig_.nodeId = network["nodeId"].get<std::string>();
    }
    if (network.contains("maxPeers") && network["maxPeers"].is_number_unsigned()) {
      networkConfig_.maxPeers = network["maxPeers"].get<uint16_t>();
    }
  }

  log().info << "Configuration loaded from " << configPath;
  log().info << "  Port: " << port_;
  return {};
}

std::string Server::handleIncomingRequest(const std::string &request) {
  auto &logger = logging::getLogger("server");
  logger.debug << "Received network request (" << request.size() << " bytes)";

  // First, try to parse as binary Client protocol
  auto clientReqResult = utl::binaryUnpack<Client::Request>(request);
  if (clientReqResult) {
    return handleClientRequest(clientReqResult.value());
  }

  logger.error << "Error handling request: " << clientReqResult.error().message;
  return R"({"error":"invalid request"})";
}

std::string Server::handleClientRequest(const Client::Request &request) {
  auto &logger = logging::getLogger("server");

  Client::Response response;
  response.version = Client::VERSION;
  response.errorCode = 0;
  response.type = request.type;

  // Check version
  if (request.version != Client::VERSION) {
    logger.warning << "Version mismatch: client=" << request.version
                   << ", server=" << Client::VERSION;
    response.errorCode = Client::E_VERSION;
    return utl::binaryPack(response);
  }

  try {
    Roe<std::string> result =
        Error(Client::E_INVALID_REQUEST,
              Client::getErrorMessage(Client::E_INVALID_REQUEST));

    switch (request.type) {
    case Client::T_REQ_INFO:
      result = handleReqInfo();
      break;

    case Client::T_REQ_QUERY_WALLET:
      result = handleReqQueryWallet(request.data);
      break;

    case Client::T_REQ_ADD_TRANSACTION:
      result = handleReqAddTransaction(request.data);
      break;

    case Client::T_REQ_BEACON_VALIDATORS:
    case Client::T_REQ_PEER_VALIDATORS:
      result = handleReqValidators();
      break;

    case Client::T_REQ_BLOCKS:
      result = handleReqBlocks(request.data);
      break;

    default:
      logger.warning << "Unknown request type: " << request.type;
      response.errorCode = Client::E_INVALID_REQUEST;
      // Note: Error message will be handled by client when it receives the
      // error code
      return utl::binaryPack(response);
    }

    if (result) {
      response.data = result.value();
    } else {
      response.errorCode = result.error().code;
      logger.error << "Handler error: " << result.error().message;
    }
  } catch (const std::exception &e) {
    logger.error << "Error handling client request: " << e.what();
    response.errorCode = Client::E_INVALID_DATA;
  }

  return utl::binaryPack(response);
}

Server::Roe<std::string> Server::handleReqInfo() {
  auto &logger = logging::getLogger("server");
  logger.debug << "Handling T_REQ_INFO";

  try {
    Client::RespInfo respData;
    respData.blockCount = ledger_.getBlockCount();
    respData.currentSlot = consensus_.getCurrentSlot();
    respData.currentEpoch = consensus_.getCurrentEpoch();
    respData.pendingTransactions = ledger_.getPendingTransactionCount();

    return utl::binaryPack(respData);
  } catch (const std::exception &e) {
    return Error(Client::E_INVALID_DATA,
                 Client::getErrorMessage(Client::E_INVALID_DATA) +
                     " Details: " + std::string(e.what()));
  }
}

Server::Roe<std::string>
Server::handleReqQueryWallet(const std::string &requestData) {
  auto &logger = logging::getLogger("server");
  logger.debug << "Handling T_REQ_QUERY_WALLET";

  auto reqDataResult = utl::binaryUnpack<Client::ReqWalletInfo>(requestData);
  if (!reqDataResult) {
    return Error(Client::E_INVALID_DATA,
                 Client::getErrorMessage(Client::E_INVALID_DATA) +
                     " Details: " + reqDataResult.error().message);
  }

  const auto &reqData = reqDataResult.value();
  auto balanceResult = ledger_.getBalance(reqData.walletId);
  if (!balanceResult) {
    return Error(Client::E_INVALID_WALLET,
                 Client::getErrorMessage(Client::E_INVALID_WALLET) +
                     " Wallet ID: " + reqData.walletId);
  }

  Client::RespWalletInfo respData;
  respData.walletId = reqData.walletId;
  respData.balance = balanceResult.value();

  return utl::binaryPack(respData);
}

Server::Roe<std::string>
Server::handleReqAddTransaction(const std::string &requestData) {
  auto &logger = logging::getLogger("server");
  logger.debug << "Handling T_REQ_ADD_TRANSACTION";

  auto reqDataResult =
      utl::binaryUnpack<Client::ReqAddTransaction>(requestData);
  if (!reqDataResult) {
    return Error(Client::E_INVALID_TRANSACTION,
                 Client::getErrorMessage(Client::E_INVALID_TRANSACTION) +
                     " Details: " + reqDataResult.error().message);
  }

  const auto &reqData = reqDataResult.value();
  // Submit transaction to the server
  submitTransaction(reqData.transaction);

  Client::RespAddTransaction respData;
  respData.transaction = reqData.transaction;

  return utl::binaryPack(respData);
}

Server::Roe<std::string> Server::handleReqValidators() {
  auto &logger = logging::getLogger("server");
  logger.debug << "Handling T_REQ_VALIDATORS";

  // Get registered stakeholders/validators from consensus
  Client::RespValidators respData;
  auto stakeholders = consensus_.getStakeholders();

  for (const auto &stakeholder : stakeholders) {
    Client::ValidatorInfo validatorInfo;
    validatorInfo.id = stakeholder.id;
    validatorInfo.stake = stakeholder.stake;
    respData.validators.push_back(validatorInfo);
  }

  return utl::binaryPack(respData);
}

Server::Roe<std::string>
Server::handleReqBlocks(const std::string &requestData) {
  auto &logger = logging::getLogger("server");
  logger.debug << "Handling T_REQ_BLOCKS";

  auto reqDataResult = utl::binaryUnpack<Client::ReqBlocks>(requestData);
  if (!reqDataResult) {
    return Error(Client::E_INVALID_DATA,
                 Client::getErrorMessage(Client::E_INVALID_DATA) +
                     " Details: " + reqDataResult.error().message);
  }

  const auto &reqData = reqDataResult.value();
  Client::RespBlocks respData;
  size_t totalBlocks = ledger_.getBlockCount();

  for (uint64_t i = reqData.fromIndex;
       i < std::min(reqData.fromIndex + reqData.count,
                    static_cast<uint64_t>(totalBlocks));
       ++i) {
    // Get block and serialize it
    auto block = ledger_.getBlock(i);
    if (block) {
      Client::BlockInfo blockInfo;
      blockInfo.index = block->getIndex();
      blockInfo.timestamp = block->getTimestamp();
      blockInfo.data = block->getData();
      blockInfo.previousHash = block->getPreviousHash();
      blockInfo.hash = block->getHash();
      blockInfo.slot = block->getSlot();
      blockInfo.slotLeader = block->getSlotLeader();
      respData.blocks.push_back(blockInfo);
    }
  }

  logger.debug << "Returning " << respData.blocks.size() << " blocks";
  return utl::binaryPack(respData);
}

void Server::broadcastBlock(std::shared_ptr<iii::Block> block) {
  if (!block) {
    return;
  }

  auto &logger = logging::getLogger("server");
  logger.info << "Broadcasting block " << block->getIndex() << " to "
              << connectedPeers_.size() << " peers";

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
    peers = std::vector<std::string>(connectedPeers_.begin(),
                                     connectedPeers_.end());
  }

  // Send to each connected peer
  for (const auto &peerAddr : peers) {
    std::string host;
    uint16_t port;
    if (parseHostPort(peerAddr, host, port)) {
      auto result = cFetch_.fetchSync(host, port, messageStr);
      if (result.isOk()) {
        logger.debug << "Sent block to peer: " << peerAddr;
      } else {
        logger.warning << "Failed to send block to peer " << peerAddr << ": "
                       << result.error().message;
      }
    }
  }
}

void Server::requestBlocksFromPeers(uint64_t fromIndex) {
  auto &logger = logging::getLogger("server");
  logger.info << "Requesting blocks from index " << fromIndex;

  std::vector<std::string> peers;
  {
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers = std::vector<std::string>(connectedPeers_.begin(),
                                     connectedPeers_.end());
  }

  for (const auto &peerAddr : peers) {
    auto blocksResult = fetchBlocksFromPeer(peerAddr, fromIndex);
    if (blocksResult && !blocksResult.value().empty()) {
      logger.info << "Received " << blocksResult.value().size()
                  << " blocks from peer";
      break; // Got blocks from one peer, that's enough
    }
  }
}

Server::Roe<std::vector<std::shared_ptr<iii::Block>>>
Server::fetchBlocksFromPeer(const std::string &hostPort, uint64_t fromIndex) {
  auto &logger = logging::getLogger("server");

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

    auto result = cFetch_.fetchSync(host, port, request.dump());
    if (!result.isOk()) {
      return Error(3, "Failed to fetch from peer: " + result.error().message);
    }

    // In full implementation:
    // 1. Parse the response JSON
    // 2. Deserialize response into Block objects

    // Placeholder return
    return std::vector<std::shared_ptr<iii::Block>>();

  } catch (const std::exception &e) {
    logger.error << "Error fetching blocks from peer: " << e.what();
    return Error(4, e.what());
  }
}

Server::Roe<std::unique_ptr<BlockChain>> Server::buildCandidateChainFromBlocks(
    const std::vector<std::shared_ptr<iii::Block>> &blocks) const {
  auto &logger = logging::getLogger("server");

  if (blocks.empty()) {
    return Error(1, "No blocks provided to build candidate chain");
  }

  // Create a new BlockChain instance
  auto candidateChain = std::make_unique<BlockChain>();

  // Convert iii::Block to Block and add to chain
  for (const auto &iBlock : blocks) {
    // Try to cast to concrete Block type
    auto block = std::dynamic_pointer_cast<Block>(iBlock);
    if (!block) {
      // If cast fails, create a new Block from the interface
      // This is a placeholder - in full implementation, we'd need proper
      // deserialization
      logger.warning << "Block cast failed, creating new Block from interface";
      block = std::make_shared<Block>();
      block->setIndex(iBlock->getIndex());
      block->setTimestamp(iBlock->getTimestamp());
      block->setData(iBlock->getData());
      block->setPreviousHash(iBlock->getPreviousHash());
      block->setHash(iBlock->getHash());
      block->setSlot(iBlock->getSlot());
      block->setSlotLeader(iBlock->getSlotLeader());
    }

    if (!candidateChain->addBlock(block)) {
      return Error(2, "Failed to add block to candidate chain");
    }
  }

  // Validate the candidate chain
  if (!candidateChain->isValid()) {
    return Error(3, "Candidate chain is invalid");
  }

  logger.info << "Built candidate chain with " << candidateChain->getSize()
              << " blocks";
  return candidateChain;
}

Server::Roe<void>
Server::switchToChain(std::unique_ptr<BlockChain> candidateChain) {
  auto &logger = logging::getLogger("server");

  if (!candidateChain) {
    return Error(1, "Candidate chain is null");
  }

  // Placeholder implementation
  // In a full implementation, this would:
  // 1. Revert transactions from current chain that aren't in candidate chain
  // 2. Apply transactions from candidate chain that aren't in current chain
  // 3. Replace the ledger's underlying blockchain
  // 4. Update wallet states to match the new chain

  logger.info << "Switching to candidate chain with "
              << candidateChain->getSize() << " blocks";
  logger.warning
      << "Chain switching is a placeholder - full implementation needed";

  // TODO: Implement full chain switching logic
  // This requires:
  // - Method in Ledger to replace the blockchain
  // - Transaction reversion logic
  // - State reconciliation

  return {};
}

Server::Roe<void> Server::initStorage(const std::string &dataDir) {
  auto &logger = logging::getLogger("server");
  logger.info << "Initializing storage in directory: " << dataDir;

  // Create data directory if it doesn't exist
  std::error_code ec;
  if (!std::filesystem::exists(dataDir, ec)) {
    if (ec) {
      return Error(2, "Failed to check data directory: " + ec.message());
    }
    if (!std::filesystem::create_directories(dataDir, ec)) {
      return Error(2, "Failed to create data directory: " + ec.message());
    }
    logger.info << "Created data directory: " << dataDir;
  } else if (ec) {
    return Error(2, "Failed to check data directory: " + ec.message());
  }

  // Set up storage paths
  Ledger::StorageConfig storageConfig;
  storageConfig.activeDirPath = dataDir + "/active";
  storageConfig.archiveDirPath = dataDir + "/archive";
  storageConfig.maxActiveDirSize = 500 * 1024 * 1024; // 500MB
  storageConfig.blockDirFileSize = 100 * 1024 * 1024; // 100MB

  // Initialize ledger storage
  auto result = ledger_.initStorage(storageConfig);
  if (!result) {
    return Error(1, "Failed to initialize ledger storage: " +
                        result.error().message);
  }

  logger.info << "Storage initialized successfully";
  logger.info << "  Active directory: " << storageConfig.activeDirPath;
  logger.info << "  Archive directory: " << storageConfig.archiveDirPath;

  return {};
}

} // namespace pp
