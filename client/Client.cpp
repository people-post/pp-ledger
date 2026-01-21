#include "Client.h"
#include "../lib/Logger.h"
#include "../network/FetchClient.h"

#include <nlohmann/json.hpp>

namespace pp {

using json = nlohmann::json;

Client::Client() : Module() {
  setLogger("client");
}

Client::~Client() { disconnect(); }

std::string Client::getErrorMessage(uint16_t errorCode) {
  switch (errorCode) {
  case E_NOT_CONNECTED:
    return "Not connected to server";
  case E_INVALID_RESPONSE:
    return "Invalid response from server";
  case E_SERVER_ERROR:
    return "Server error";
  case E_PARSE_ERROR:
    return "Failed to parse response";
  case E_REQUEST_FAILED:
    return "Request failed";
  default:
    return "Unknown error";
  }
}

bool Client::initBeacon(const network::TcpEndpoint &endpoint) {
  endpoint_ = endpoint;
  connected_ = true;
  log().info << "Connected to BeaconServer at " << endpoint_.address << ":"
             << endpoint_.port;
  return true;
}

bool Client::initBeacon(const std::string &address, uint16_t port) {
  return initBeacon(network::TcpEndpoint{address, port});
}

bool Client::initMiner(const network::TcpEndpoint &endpoint) {
  endpoint_ = endpoint;
  connected_ = true;
  log().info << "Connected to MinerServer at " << endpoint_.address << ":"
             << endpoint_.port;
  return true;
}

bool Client::initMiner(const std::string &address, uint16_t port) {
  return initMiner(network::TcpEndpoint{address, port});
}

void Client::disconnect() {
  if (connected_) {
    log().info << "Disconnected from server";
    connected_ = false;
  }
}

bool Client::isConnected() const { return connected_; }

Client::Roe<json> Client::sendRequest(const json &request) {
  if (!connected_) {
    return Error(E_NOT_CONNECTED, getErrorMessage(E_NOT_CONNECTED));
  }

  // Serialize request to string
  std::string requestData = request.dump();

  log().debug << "Sending request: " << requestData;

  // Send request using FetchClient
  network::FetchClient fetchClient;
  auto result = fetchClient.fetchSync(endpoint_, requestData);

  if (!result.isOk()) {
    log().error << "Failed to send request: " << result.error().message;
    return Error(E_REQUEST_FAILED,
                 getErrorMessage(E_REQUEST_FAILED) + ": " +
                     result.error().message);
  }

  // Parse JSON response
  json response;
  try {
    response = json::parse(result.value());
  } catch (const json::exception &e) {
    log().error << "Failed to parse response: " << e.what();
    return Error(E_PARSE_ERROR, getErrorMessage(E_PARSE_ERROR) + ": " + e.what());
  }

  log().debug << "Received response: " << response.dump();

  // Check for error in response
  if (response.contains("status") && response["status"] == "error") {
    std::string errorMsg = response.value("error", "Unknown server error");
    return Error(E_SERVER_ERROR, getErrorMessage(E_SERVER_ERROR) + ": " + errorMsg);
  }

  return response;
}

// BeaconServer API - Block operations

Client::Roe<Client::BlockInfo> Client::getBlock(uint64_t blockId) {
  log().debug << "Requesting block " << blockId;

  json request = {{"type", "block"}, {"action", "get"}, {"blockId", blockId}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("block")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'block' field");
  }

  const json &blockJson = response["block"];

  BlockInfo block;
  block.index = blockJson.value("index", 0);
  block.timestamp = blockJson.value("timestamp", 0);
  block.data = blockJson.value("data", "");
  block.previousHash = blockJson.value("previousHash", "");
  block.hash = blockJson.value("hash", "");
  block.slot = blockJson.value("slot", 0);
  block.slotLeader = blockJson.value("slotLeader", "");

  return block;
}

Client::Roe<uint64_t> Client::getCurrentBlockId() {
  log().debug << "Requesting current block ID";

  json request = {{"type", "block"}, {"action", "current"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("blockId")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'blockId' field");
  }

  return response["blockId"].get<uint64_t>();
}

Client::Roe<bool> Client::addBlock(const BlockInfo &block) {
  log().debug << "Adding block " << block.index;

  json blockJson = {{"index", block.index},
                    {"timestamp", block.timestamp},
                    {"data", block.data},
                    {"previousHash", block.previousHash},
                    {"hash", block.hash},
                    {"slot", block.slot},
                    {"slotLeader", block.slotLeader}};

  json request = {{"type", "block"}, {"action", "add"}, {"block", blockJson}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();
  return response.value("status", "") == "ok";
}

// BeaconServer API - Stakeholder operations

Client::Roe<std::vector<Client::StakeholderInfo>>
Client::listStakeholders() {
  log().debug << "Requesting stakeholder list";

  json request = {{"type", "stakeholder"}, {"action", "list"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("stakeholders")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'stakeholders' field");
  }

  std::vector<StakeholderInfo> stakeholders;
  for (const auto &item : response["stakeholders"]) {
    StakeholderInfo info;
    info.id = item.value("id", "");
    info.stake = item.value("stake", 0);
    stakeholders.push_back(info);
  }

  return stakeholders;
}

// BeaconServer API - Consensus queries

Client::Roe<uint64_t> Client::getCurrentSlot() {
  log().debug << "Requesting current slot";

  json request = {{"type", "consensus"}, {"action", "currentSlot"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("slot")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'slot' field");
  }

  return response["slot"].get<uint64_t>();
}

Client::Roe<uint64_t> Client::getCurrentEpoch() {
  log().debug << "Requesting current epoch";

  json request = {{"type", "consensus"}, {"action", "currentEpoch"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("epoch")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'epoch' field");
  }

  return response["epoch"].get<uint64_t>();
}

Client::Roe<std::string> Client::getSlotLeader(uint64_t slot) {
  log().debug << "Requesting slot leader for slot " << slot;

  json request = {{"type", "consensus"}, {"action", "slotLeader"}, {"slot", slot}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("slotLeader")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'slotLeader' field");
  }

  return response["slotLeader"].get<std::string>();
}

// MinerServer API - Transaction operations

Client::Roe<bool> Client::addTransaction(const json &transaction) {
  log().debug << "Adding transaction";

  json request = {{"type", "transaction"}, {"action", "add"}, {"transaction", transaction}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();
  return response.value("status", "") == "ok";
}

Client::Roe<uint64_t> Client::getPendingTransactionCount() {
  log().debug << "Requesting pending transaction count";

  json request = {{"type", "transaction"}, {"action", "count"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("count")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'count' field");
  }

  return response["count"].get<uint64_t>();
}

// MinerServer API - Mining operations

Client::Roe<bool> Client::produceBlock() {
  log().debug << "Requesting block production";

  json request = {{"type", "mining"}, {"action", "produce"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();
  return response.value("status", "") == "ok";
}

Client::Roe<bool> Client::shouldProduceBlock() {
  log().debug << "Checking if should produce block";

  json request = {{"type", "mining"}, {"action", "shouldProduce"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("shouldProduce")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'shouldProduce' field");
  }

  return response["shouldProduce"].get<bool>();
}

// MinerServer API - Status

Client::Roe<Client::MinerStatus> Client::getMinerStatus() {
  log().debug << "Requesting miner status";

  json request = {{"type", "status"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  MinerStatus status;
  status.minerId = response.value("minerId", "");
  status.stake = response.value("stake", 0);
  status.currentBlockId = response.value("currentBlockId", 0);
  status.currentSlot = response.value("currentSlot", 0);
  status.currentEpoch = response.value("currentEpoch", 0);
  status.pendingTransactions = response.value("pendingTransactions", 0);
  status.isSlotLeader = response.value("isSlotLeader", false);

  return status;
}

} // namespace pp
