#include "Client.h"
#include "../lib/Logger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Utilities.h"
#include "../network/FetchClient.h"

#include <nlohmann/json.hpp>

namespace pp {

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

Client::Roe<nlohmann::json> Client::sendRequest(const nlohmann::json &request) {
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

  // Unpack binary Response
  auto respResult = utl::binaryUnpack<Response>(result.value());
  if (!respResult) {
    log().error << "Failed to unpack response: " << respResult.error().message;
    return Error(E_PARSE_ERROR, getErrorMessage(E_PARSE_ERROR) + ": " + respResult.error().message);
  }

  const Response& resp = respResult.value();
  log().debug << "Received response: errorCode=" << resp.errorCode;

  if (resp.errorCode != 0) {
    return Error(E_SERVER_ERROR, getErrorMessage(E_SERVER_ERROR) + ": " + resp.payload);
  }

  log().debug << "Response payload: " << resp.payload.size() << " bytes";

  nlohmann::json response;
  try {
    response = nlohmann::json::parse(resp.payload);
  } catch (const nlohmann::json::exception &e) {
    log().error << "Failed to parse response payload: " << e.what();
    return Error(E_PARSE_ERROR, getErrorMessage(E_PARSE_ERROR) + ": " + e.what());
  }

  return response;
}

Client::Roe<std::string> Client::sendBinaryRequest(uint32_t type, const std::string &payload) {
  if (endpoint_.port == 0) {
    return Error(E_NOT_CONNECTED, getErrorMessage(E_NOT_CONNECTED));
  }

  Request req;
  req.version = Request::VERSION;
  req.type = type;
  req.payload = payload;

  std::string requestData = utl::binaryPack(req);
  log().debug << "Sending binary request: " << req;

  network::FetchClient fetchClient;
  fetchClient.redirectLogger(log().getFullName() + ".FetchClient");
  auto result = fetchClient.fetchSync(endpoint_, requestData);

  if (!result.isOk()) {
    return Error(E_REQUEST_FAILED,
                 getErrorMessage(E_REQUEST_FAILED) + ": " +
                     result.error().message);
  }
  return result.value();
}

// BeaconServer API - Block operations (binary T_REQ_BLOCK_GET)

Client::Roe<Ledger::ChainNode> Client::fetchBlock(uint64_t blockId) {
  log().debug << "Requesting block " << blockId;

  std::string payload = utl::binaryPack(blockId);
  auto result = sendBinaryRequest(T_REQ_BLOCK_GET, payload);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto respResult = utl::binaryUnpack<Response>(result.value());
  if (!respResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack block response");
  }
  const Response &resp = respResult.value();
  if (resp.isError()) {
    return Error(E_SERVER_ERROR, resp.payload.empty() ? "Block get failed" : resp.payload);
  }

  Ledger::ChainNode node;
  if (!node.ltsFromString(resp.payload)) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize block");
  }
  return node;
}

Client::Roe<Client::BeaconState> Client::registerMinerServer(const network::TcpEndpoint &endpoint) {
  log().debug << "Registering miner server: " << endpoint;

  nlohmann::json request = {{"type", "register"}, {"address", endpoint.toString()}};
  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const nlohmann::json &response = result.value();
  BeaconState state;
  state.lastCheckpointId = response["lastCheckpointId"].get<uint64_t>();
  state.checkpointId = response["currentCheckpointId"].get<uint64_t>();
  state.nextBlockId = response["nextBlockId"].get<uint64_t>();
  state.currentSlot = response["currentSlot"].get<uint64_t>();
  state.currentEpoch = response["currentEpoch"].get<uint64_t>();
  state.currentTimestamp = response["currentTimestamp"].get<int64_t>();

  return state;
}

Client::Roe<Client::BeaconState> Client::fetchBeaconState() {
  log().debug << "Requesting beacon state (checkpoint, block)";

  nlohmann::json request = {{"type", "state"}, {"action", "current"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const nlohmann::json &response = result.value();

  if (!response.contains("currentCheckpointId") || !response.contains("nextBlockId") ||
      !response.contains("currentSlot") || !response.contains("currentEpoch") ||
      !response.contains("currentTimestamp")) {
    return Error(E_INVALID_RESPONSE,
                 "Response missing 'currentCheckpointId', 'nextBlockId', "
                 "'currentSlot', 'currentEpoch', 'currentTimestamp'");
  }

  BeaconState state;
  state.lastCheckpointId = response["lastCheckpointId"].get<uint64_t>();
  state.checkpointId = response["currentCheckpointId"].get<uint64_t>();
  state.nextBlockId = response["nextBlockId"].get<uint64_t>();
  state.currentSlot = response["currentSlot"].get<uint64_t>();
  state.currentEpoch = response["currentEpoch"].get<uint64_t>();
  state.currentTimestamp = response["currentTimestamp"].get<int64_t>();

  return state;
}

Client::Roe<bool> Client::addBlock(const Ledger::ChainNode& block) {
  log().debug << "Adding block " << block.block.index;

  std::string payload = block.ltsToString();
  auto result = sendBinaryRequest(T_REQ_BLOCK_ADD, payload);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto respResult = utl::binaryUnpack<Response>(result.value());
  if (!respResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack block response");
  }
  const Response &resp = respResult.value();
  if (resp.isError()) {
    return Error(E_SERVER_ERROR, resp.payload.empty() ? "Block add failed" : resp.payload);
  }
  return true;
}

Client::Roe<std::vector<consensus::Stakeholder>>
Client::fetchStakeholders() {
  log().debug << "Requesting stakeholder list";

  nlohmann::json request = {{"type", "stakeholder"}, {"action", "list"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const nlohmann::json &response = result.value();

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

  nlohmann::json request = {{"type", "consensus"}, {"action", "slotLeader"}, {"slot", slot}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const nlohmann::json &response = result.value();

  if (!response.contains("slotLeader")) {
    return Error(E_INVALID_RESPONSE, "Response missing 'slotLeader' field");
  }

  return response["slotLeader"].get<uint64_t>();
}

// MinerServer API - Transaction operations

Client::Roe<void> Client::addTransaction(const Ledger::Transaction &transaction) {
  log().debug << "Adding transaction";

  std::string payload = utl::binaryPack(transaction);
  auto result = sendBinaryRequest(T_REQ_TRANSACTION_ADD, payload);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto respResult = utl::binaryUnpack<Response>(result.value());
  if (!respResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack transaction response");
  }
  const Response &resp = respResult.value();
  if (resp.isError()) {
    return Error(E_SERVER_ERROR, resp.payload.empty() ? "Transaction add failed" : resp.payload);
  }
  return {};
}

// MinerServer API - Status

Client::Roe<Client::MinerStatus> Client::fetchMinerStatus() {
  log().debug << "Requesting miner status";

  nlohmann::json request = {{"type", "status"}};

  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const nlohmann::json &response = result.value();

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
