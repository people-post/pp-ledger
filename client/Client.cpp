#include "Client.h"
#include "../lib/Logger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Utilities.h"
#include "../network/FetchClient.h"

#include <nlohmann/json.hpp>

namespace pp {

using json = nlohmann::json;

Client::Client() {}

Client::~Client() {}

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

Client::Roe<void> Client::setEndpoint(const std::string& endpoint) {
  auto ep = network::TcpEndpoint::fromString(endpoint);
  if (ep.port == 0) {
    return Error(E_NOT_CONNECTED, "Invalid endpoint: " + endpoint);
  }
  endpoint_ = ep;
  return {};
}

void Client::setEndpoint(const network::TcpEndpoint &endpoint) {
  endpoint_ = endpoint;
}

Client::Roe<json> Client::sendRequest(const json &request) {
  if (endpoint_.port == 0) {
    return Error(E_NOT_CONNECTED, getErrorMessage(E_NOT_CONNECTED));
  }

  // Wrap JSON in Request struct with type = T_REQ_JSON
  Request req;
  req.version = Request::VERSION;
  req.type = T_REQ_JSON;
  req.payload = request.dump();

  std::string requestData = utl::binaryPack(req);
  log().debug << "Sending request: " << req;

  // Send request using FetchClient
  network::FetchClient fetchClient;
  fetchClient.redirectLogger(log().getFullName() + ".FetchClient");
  auto result = fetchClient.fetchSync(endpoint_, requestData);

  if (!result.isOk()) {
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

Client::Roe<Ledger::ChainNode> Client::fetchBlock(uint64_t blockId) {
  log().debug << "Requesting block " << blockId;

  json request = {{"type", "block"}, {"action", "get"}, {"blockId", blockId}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (response.contains("error")) {
    return Error(E_SERVER_ERROR, response["error"].get<std::string>());
  }

  if (!response.contains("block")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'block' field");
  }
  if (!response["block"].is_string()) {
    return Error(E_INVALID_RESPONSE, "block field must be hex string");
  }

  std::string hex = response["block"].get<std::string>();
  std::string binary = utl::fromJsonSafeString(hex);
  Ledger::ChainNode node;
  if (!node.ltsFromString(binary)) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize block");
  }
  return node;
}

Client::Roe<uint64_t> Client::fetchNextBlockId() {
  log().debug << "Requesting current block ID";

  json request = {{"type", "block"}, {"action", "next"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("nextBlockId")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'nextBlockId' field");
  }

  return response["nextBlockId"].get<uint64_t>();
}

Client::Roe<Client::BeaconState> Client::fetchBeaconState() {
  log().debug << "Requesting beacon state (checkpoint, block, stakeholders)";

  json request = {{"type", "state"}, {"action", "current"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  if (!response.contains("currentCheckpointId") || !response.contains("nextBlockId") ||
      !response.contains("currentSlot") || !response.contains("currentEpoch") ||
      !response.contains("currentTimestamp") || !response.contains("stakeholders")) {
    return Error(E_INVALID_RESPONSE,
                 "Response missing 'currentCheckpointId', 'nextBlockId', "
                 "'currentSlot', 'currentEpoch', 'currentTimestamp' or 'stakeholders'");
  }

  BeaconState state;
  state.checkpointId = response["currentCheckpointId"].get<uint64_t>();
  state.nextBlockId = response["nextBlockId"].get<uint64_t>();
  state.currentSlot = response["currentSlot"].get<uint64_t>();
  state.currentEpoch = response["currentEpoch"].get<uint64_t>();
  state.currentTimestamp = response["currentTimestamp"].get<int64_t>();
  state.stakeholders.clear();
  for (const auto &item : response["stakeholders"]) {
    consensus::Stakeholder info;
    info.id = item.value("id", 0);
    info.stake = item.value("stake", 0);
    state.stakeholders.push_back(info);
  }

  return state;
}

Client::Roe<bool> Client::addBlock(const Ledger::ChainNode& block) {
  log().debug << "Adding block " << block.block.index;

  json request = {{"type", "block"}, {"action", "add"}, {"block", utl::toJsonSafeString(block.ltsToString())}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();
  return response.value("status", "") == "ok";
}

// BeaconServer API - Stakeholder operations

Client::Roe<std::vector<consensus::Stakeholder>>
Client::fetchStakeholders() {
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

  std::vector<consensus::Stakeholder> stakeholders;
  for (const auto &item : response["stakeholders"]) {
    consensus::Stakeholder info;
    info.id = item.value("id", 0);
    info.stake = item.value("stake", 0);
    stakeholders.push_back(info);
  }

  return stakeholders;
}

Client::Roe<uint64_t> Client::fetchSlotLeader(uint64_t slot) {
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

  return response["slotLeader"].get<uint64_t>();
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

Client::Roe<uint64_t> Client::fetchPendingTransactionCount() {
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

Client::Roe<bool> Client::fetchIsSlotLeader() {
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

Client::Roe<Client::MinerStatus> Client::fetchMinerStatus() {
  log().debug << "Requesting miner status";

  json request = {{"type", "status"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const json &response = result.value();

  MinerStatus status;
  status.minerId = response.value("minerId", 0);
  status.stake = response.value("stake", 0);
  status.nextBlockId = response.value("nextBlockId", 0);
  status.currentSlot = response.value("currentSlot", 0);
  status.currentEpoch = response.value("currentEpoch", 0);
  status.pendingTransactions = response.value("pendingTransactions", 0);
  status.isSlotLeader = response.value("isSlotLeader", false);

  return status;
}

} // namespace pp
