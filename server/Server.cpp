#include "Server.h"
#include "../client/Client.h"
#include "../ledger/Block.h"
#include "BinaryPack.hpp"
#include "Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace pp {

Server::Server() : Service("server") {
  log().info << "Server initialized";
}

bool Server::start(const std::string &dataDir) {
  if (isRunning()) {
    log().warning << "Server is already running on port " << config_.network.port;
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

  log().addFileHandler(dataDir + "/server.log", logging::Level::DEBUG);

  // Initialize ledger first
  auto ledgerResult = initLedger(dataDir);
  if (!ledgerResult) {
    log().error << "Failed to initialize ledger: "
                << ledgerResult.error().message;
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
  // Start fetch server (listens on main port for client connections)
  network::FetchServer::Config fetchServerConfig;
  fetchServerConfig.host = config_.network.host;
  fetchServerConfig.port = config_.network.port;
  fetchServerConfig.handler = [this](const std::string &request) {
    return handleIncomingRequest(request);
  };
  bool fetchServerStarted = sFetch_.start(fetchServerConfig);

  if (!fetchServerStarted) {
    log().error << "Failed to start fetch server on " << config_.network.host << ":" << config_.network.port;
    return false;
  }

  log().info << "Fetch server started on " << config_.network.host << ":" << config_.network.port;
  return true;
}

void Server::onStop() {
  // Stop client server
  sFetch_.stop();
}

void Server::addTransaction(const std::string &transaction) {
  // Deserialize transaction string to Transaction struct using utility function
  auto txResult = utl::binaryUnpack<Ledger::Transaction>(transaction);
  if (!txResult) {
    log().error << "Failed to deserialize transaction: "
                 << txResult.error().message;
    return;
  }

  auto result = ledger_.addTransaction(txResult.value());
  if (!result) {
    log().error << "Failed to add transaction: " << result.error().message;
    return;
  }

  log().debug << "Transaction submitted (fromWallet: "
               << txResult.value().fromWallet
               << ", toWallet: " << txResult.value().toWallet
               << ", amount: " << txResult.value().amount << ")";
}

bool Server::shouldProduceBlock() const {
  // Produce block if we have pending transactions
  if (ledger_.getPendingTransactionCount() == 0) {
    return false;
  }

  // In multi-node setup, check if we are the slot leader
  if (!config_.network.nodeId.empty()) {
    uint64_t currentSlot = consensus_.getCurrentSlot();
    auto slotLeaderResult = consensus_.getSlotLeader(currentSlot);

    if (slotLeaderResult && slotLeaderResult.value() == config_.network.nodeId) {
      return true;
    }
    return false;
  }

  // For single-node setup, always produce if there are pending transactions
  return true;
}

Server::Roe<void> Server::produceBlock() {
  if (!shouldProduceBlock()) {
    return Server::Roe<void>();
  }

  // Get current slot and slot leader
  uint64_t currentSlot = consensus_.getCurrentSlot();
  auto slotLeaderResult = consensus_.getSlotLeader(currentSlot);
  if (!slotLeaderResult) {
    return Error(1, "Failed to get slot leader: " +
                        slotLeaderResult.error().message);
  }

  // Create validator function that wraps consensus validation
  auto validator = [this](const iii::Block &block,
                          const iii::BlockChain &chain) -> Ledger::Roe<bool> {
    auto validateResult = consensus_.validateBlock(block, chain);
    if (!validateResult) {
      return Ledger::Error(validateResult.error().code,
                           validateResult.error().message);
    }
    if (!validateResult.value()) {
      return Ledger::Error(1, "Block did not pass consensus validation");
    }
    return true;
  };

  // Produce block using Ledger (creates, validates, adds, and returns serialized block)
  auto serializedBlockResult =
      ledger_.produceBlock(currentSlot, slotLeaderResult.value(), validator);
  if (!serializedBlockResult) {
    return Error(serializedBlockResult.error().code,
                 serializedBlockResult.error().message);
  }

  // Get the serialized block string for broadcasting
  std::string serializedBlock = serializedBlockResult.value();

  // Broadcast serialized block to network peers
  // TODO: Implement block broadcasting with serialized block string
  // The serializedBlock string is now available for broadcasting
  log().info << "Block produced and added to ledger (slot: " << currentSlot
             << ", leader: " << slotLeaderResult.value()
             << ", serialized size: " << serializedBlock.size() << " bytes)";

  return Server::Roe<void>();
}

Server::Roe<void> Server::syncState() {
  // Get our current block count
  size_t localBlockCount = ledger_.getBlockCount();

  // Request blocks from peers starting from our current height
  std::vector<std::string> peers;

  for (const auto &peerAddr : peers) {
    auto blocksResult = fetchBlocksFromPeer(peerAddr, localBlockCount);
    if (blocksResult && !blocksResult.value().empty()) {
      log().info << "Fetched " << blocksResult.value().size()
                  << " blocks from peer";

      // Build candidate chain from received blocks
      auto candidateChainResult =
          buildCandidateChainFromBlocks(blocksResult.value());
      if (!candidateChainResult) {
        log().warning << "Failed to build candidate chain: "
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
            log().warning << "Block " << block->getIndex()
                           << " in candidate chain failed validation";
            allBlocksValid = false;
            break;
          }
        }
      }

      if (!allBlocksValid) {
        log().warning << "Candidate chain contains invalid blocks, skipping";
        continue;
      }

      // Check if we should switch to this candidate chain
      auto switchResult =
          consensus_.shouldSwitchChain(ledger_, *candidateChain);
      if (!switchResult) {
        log().warning << "Chain switch check failed: "
                       << switchResult.error().message;
        continue;
      }

      if (switchResult.value()) {
        log().info << "Candidate chain is longer and valid, switching chains";
        auto switchChainResult = switchToChain(std::move(candidateChain));
        if (!switchChainResult) {
          log().error << "Failed to switch chains: "
                       << switchChainResult.error().message;
        } else {
          log().info << "Successfully switched to longer chain";
          // Break after successful switch to avoid processing more peers
          break;
        }
      } else {
        log().debug << "Current chain is longer or equal, not switching";
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

  // Load host (optional, default: "localhost")
  if (config.contains("host") && config["host"].is_string()) {
    config_.network.host = config["host"].get<std::string>();
  } else {
    config_.network.host = Client::DEFAULT_HOST;
  }

  // Load port (optional, default: 8517)
  if (config.contains("port") && config["port"].is_number()) {
    if (config["port"].is_number_integer()) {
      config_.network.port = config["port"].get<int>();
    } else {
      return Error(3, "Configuration file 'port' field is not an integer");
    }
  } else {
    config_.network.port = Client::DEFAULT_PORT;
  }

  // Load network configuration (optional, defaults will be used if not
  // present)
  if (config.contains("network") && config["network"].is_object()) {
    const auto &network = config["network"];
    if (network.contains("nodeId") && network["nodeId"].is_string()) {
      config_.network.nodeId = network["nodeId"].get<std::string>();
    }
    if (network.contains("maxPeers") && network["maxPeers"].is_number_unsigned()) {
      config_.network.maxPeers = network["maxPeers"].get<uint16_t>();
    }
  }

  log().info << "Configuration loaded from " << configPath;
  log().info << "  Host: " << config_.network.host;
  log().info << "  Port: " << config_.network.port;
  return {};
}

std::string Server::handleIncomingRequest(const std::string &request) {
  log().debug << "Received network request (" << request.size() << " bytes)";

  // First, try to parse as binary Client protocol
  auto clientReqResult = utl::binaryUnpack<Client::Request>(request);
  if (clientReqResult) {
    return handleClientRequest(clientReqResult.value());
  }

  log().error << "Error handling request: " << clientReqResult.error().message;
  return R"({"error":"invalid request"})";
}

std::string Server::handleClientRequest(const Client::Request &request) {
  Client::Response response;
  response.version = Client::VERSION;
  response.errorCode = 0;
  response.type = request.type;

  // Check version
  if (request.version != Client::VERSION) {
    log().warning << "Version mismatch: client=" << request.version
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
      log().warning << "Unknown request type: " << request.type;
      response.errorCode = Client::E_INVALID_REQUEST;
      // Note: Error message will be handled by client when it receives the
      // error code
      return utl::binaryPack(response);
    }

    if (result) {
      response.data = result.value();
    } else {
      response.errorCode = result.error().code;
      log().error << "Handler error: " << result.error().message;
    }
  } catch (const std::exception &e) {
    log().error << "Error handling client request: " << e.what();
    response.errorCode = Client::E_INVALID_DATA;
  }

  return utl::binaryPack(response);
}

Server::Roe<std::string> Server::handleReqInfo() {
  log().debug << "Handling T_REQ_INFO";

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
  log().debug << "Handling T_REQ_QUERY_WALLET";

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
  log().debug << "Handling T_REQ_ADD_TRANSACTION";

  auto reqDataResult =
      utl::binaryUnpack<Client::ReqAddTransaction>(requestData);
  if (!reqDataResult) {
    return Error(Client::E_INVALID_TRANSACTION,
                 Client::getErrorMessage(Client::E_INVALID_TRANSACTION) +
                     " Details: " + reqDataResult.error().message);
  }

  const auto &reqData = reqDataResult.value();
  addTransaction(reqData.transaction);

  Client::RespAddTransaction respData;
  respData.transaction = reqData.transaction;

  return utl::binaryPack(respData);
}

Server::Roe<std::string> Server::handleReqValidators() {
  log().debug << "Handling T_REQ_VALIDATORS";

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
  log().debug << "Handling T_REQ_BLOCKS";

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

  log().debug << "Returning " << respData.blocks.size() << " blocks";
  return utl::binaryPack(respData);
}

void Server::broadcastBlock(std::shared_ptr<iii::Block> block) {
  // TODO: Implement block broadcasting
}

void Server::requestBlocksFromPeers(uint64_t fromIndex) {
  // TODO: Implement block request from peers
}

Server::Roe<std::vector<std::shared_ptr<iii::Block>>>
Server::fetchBlocksFromPeer(const std::string &hostPort, uint64_t fromIndex) {
  std::string host;
  uint16_t port;
  if (!utl::parseHostPort(hostPort, host, port)) {
    return Error(2, "Invalid peer address format");
  }

  // Create request
  nlohmann::json request;
  request["type"] = "get_blocks";
  request["from_index"] = fromIndex;
  request["count"] = 100;

  try {
    log().debug << "Fetching blocks from peer: " << hostPort;

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
    log().error << "Error fetching blocks from peer: " << e.what();
    return Error(4, e.what());
  }
}

Server::Roe<std::unique_ptr<BlockChain>> Server::buildCandidateChainFromBlocks(
    const std::vector<std::shared_ptr<iii::Block>> &blocks) const {
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
      log().warning << "Block cast failed, creating new Block from interface";
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

  log().info << "Built candidate chain with " << candidateChain->getSize()
              << " blocks";
  return candidateChain;
}

Server::Roe<void>
Server::switchToChain(std::unique_ptr<BlockChain> candidateChain) {
  if (!candidateChain) {
    return Error(1, "Candidate chain is null");
  }

  // Placeholder implementation
  // In a full implementation, this would:
  // 1. Revert transactions from current chain that aren't in candidate chain
  // 2. Apply transactions from candidate chain that aren't in current chain
  // 3. Replace the ledger's underlying blockchain
  // 4. Update wallet states to match the new chain

  log().info << "Switching to candidate chain with "
              << candidateChain->getSize() << " blocks";
  log().warning
      << "Chain switching is a placeholder - full implementation needed";

  // TODO: Implement full chain switching logic
  // This requires:
  // - Method in Ledger to replace the blockchain
  // - Transaction reversion logic
  // - State reconciliation

  return {};
}

Server::Roe<void> Server::initLedger(const std::string &dataDir) {
  log().info << "Initializing ledger in directory: " << dataDir;

  // Create data directory if it doesn't exist
  std::error_code ec;
  if (!std::filesystem::exists(dataDir, ec)) {
    if (ec) {
      return Error(2, "Failed to check data directory: " + ec.message());
    }
    if (!std::filesystem::create_directories(dataDir, ec)) {
      return Error(2, "Failed to create data directory: " + ec.message());
    }
    log().info << "Created data directory: " << dataDir;
  } else if (ec) {
    return Error(2, "Failed to check data directory: " + ec.message());
  }

  // Set up storage paths
  Ledger::Config config;
  config.storage.activeDirPath = dataDir + "/active";
  config.storage.archiveDirPath = dataDir + "/archive";
  config.storage.maxActiveDirSize = 500 * 1024 * 1024; // 500MB
  config.storage.blockDirFileSize = 100 * 1024 * 1024; // 100MB

  // Initialize ledger storage
  auto result = ledger_.init(config);
  if (!result) {
    return Error(1, "Failed to initialize ledger storage: " +
                        result.error().message);
  }

  log().info << "Ledger initialized successfully";
  log().info << "  Active directory: " << config.storage.activeDirPath;
  log().info << "  Archive directory: " << config.storage.archiveDirPath;

  return {};
}

} // namespace pp
