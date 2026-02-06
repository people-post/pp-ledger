#include "Client.h"
#include "../lib/Logger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Serialize.hpp"
#include "../lib/Utilities.h"
#include "../network/FetchClient.h"

#include <nlohmann/json.hpp>
#include <sstream>

namespace pp {

std::string Client::AccountInfo::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Client::AccountInfo::ltsFromString(const std::string& str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

nlohmann::json Client::AccountInfo::toJson() const {
  nlohmann::json j;
  nlohmann::json balances;
  for (const auto& [tokenId, balance] : mBalances) {
    balances[std::to_string(tokenId)] = balance;
  }
  j["mBalances"] = balances;
  nlohmann::json keysArray = nlohmann::json::array();
  for (const auto& pk : publicKeys) {
    keysArray.push_back(utl::toJsonSafeString(pk));
  }
  j["publicKeys"] = keysArray;
  j["meta"] = utl::toJsonSafeString(meta);
  return j;
}

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

Client::Roe<Client::AccountInfo> Client::fetchAccountInfo(const uint64_t accountId) {
  log().debug << "Requesting account info for account " << accountId;

  std::string payload = utl::binaryPack(accountId);
  auto result = sendBinaryRequest(T_REQ_ACCOUNT_GET, payload);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto respResult = utl::binaryUnpack<Response>(result.value());
  if (!respResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack account info response");
  }
  const Response &resp = respResult.value();
  if (resp.isError()) {
    return Error(E_SERVER_ERROR, resp.payload.empty() ? "Account info get failed" : resp.payload);
  }

  AccountInfo info;
  if (!info.ltsFromString(resp.payload)) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize account info");
  }
  return info;
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
  auto parseResult = state.ltsFromJson(response);
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }

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

  BeaconState state;
  auto parseResult = state.ltsFromJson(response);
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }

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

Client::Roe<void> Client::addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  log().debug << "Adding transaction";

  std::string payload = utl::binaryPack(signedTx);
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
  auto parseResult = status.ltsFromJson(response);
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }

  return status;
}

// MinerStatus serialization

nlohmann::json Client::MinerStatus::ltsToJson() const {
  nlohmann::json j;
  j["minerId"] = minerId;
  j["stake"] = stake;
  j["nextBlockId"] = nextBlockId;
  j["currentSlot"] = currentSlot;
  j["currentEpoch"] = currentEpoch;
  j["pendingTransactions"] = pendingTransactions;
  j["nStakeholders"] = nStakeholders;
  j["isSlotLeader"] = isSlotLeader;
  return j;
}

Client::Roe<bool> Client::MinerStatus::ltsFromJson(const nlohmann::json& json) {
  try {
    if (!json.is_object()) {
      return Error(E_PARSE_ERROR, "MinerStatus JSON must be an object");
    }

    minerId = json.value("minerId", uint64_t(0));
    stake = json.value("stake", uint64_t(0));
    nextBlockId = json.value("nextBlockId", uint64_t(0));
    currentSlot = json.value("currentSlot", uint64_t(0));
    currentEpoch = json.value("currentEpoch", uint64_t(0));
    pendingTransactions = json.value("pendingTransactions", uint64_t(0));
    nStakeholders = json.value("nStakeholders", uint64_t(0));
    isSlotLeader = json.value("isSlotLeader", false);

    return true;
  } catch (const std::exception& e) {
    return Error(E_PARSE_ERROR, std::string("Failed to parse MinerStatus JSON: ") + e.what());
  }
}

// BeaconState serialization

nlohmann::json Client::BeaconState::ltsToJson() const {
  nlohmann::json j;
  j["currentTimestamp"] = currentTimestamp;
  j["lastCheckpointId"] = lastCheckpointId;
  j["checkpointId"] = checkpointId;
  j["nextBlockId"] = nextBlockId;
  j["currentSlot"] = currentSlot;
  j["currentEpoch"] = currentEpoch;
  j["nStakeholders"] = nStakeholders;
  return j;
}

Client::Roe<bool> Client::BeaconState::ltsFromJson(const nlohmann::json& json) {
  try {
    if (!json.is_object()) {
      return Error(E_PARSE_ERROR, "BeaconState JSON must be an object");
    }

    currentTimestamp = json.value("currentTimestamp", int64_t(0));
    lastCheckpointId = json.value("lastCheckpointId", uint64_t(0));
    checkpointId = json.value("checkpointId", uint64_t(0));
    nextBlockId = json.value("nextBlockId", uint64_t(0));
    currentSlot = json.value("currentSlot", uint64_t(0));
    currentEpoch = json.value("currentEpoch", uint64_t(0));
    nStakeholders = json.value("nStakeholders", uint64_t(0));

    return true;
  } catch (const std::exception& e) {
    return Error(E_PARSE_ERROR, std::string("Failed to parse BeaconState JSON: ") + e.what());
  }
}

} // namespace pp
