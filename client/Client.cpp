#include "Client.h"
#include "../lib/Logger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Serialize.hpp"
#include "../lib/Utilities.h"

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

Client::Client() {
  fetchClient_.redirectLogger(log().getFullName() + ".FetchClient");
}

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
  auto ep = network::TcpEndpoint::ltsFromString(endpoint);
  if (ep.port == 0) {
    return Error(E_NOT_CONNECTED, "Invalid endpoint: " + endpoint);
  }
  endpoint_ = ep;
  return {};
}

void Client::setEndpoint(const network::TcpEndpoint &endpoint) {
  endpoint_ = endpoint;
}

Client::Roe<std::string> Client::sendRequest(uint32_t type, const std::string &payload) {
  if (endpoint_.port == 0) {
    return Error(E_NOT_CONNECTED, getErrorMessage(E_NOT_CONNECTED));
  }

  Request req;
  req.version = Request::VERSION;
  req.type = type;
  req.payload = payload;

  std::string requestData = utl::binaryPack(req);
  log().debug << "Sending binary request: " << req;

  auto result = fetchClient_.fetchSync(endpoint_, requestData);

  if (!result.isOk()) {
    return Error(E_REQUEST_FAILED,
                 getErrorMessage(E_REQUEST_FAILED) + ": " +
                     result.error().message);
  }
  auto respResult = utl::binaryUnpack<Response>(result.value());
  if (!respResult) {
    return Error(E_INVALID_RESPONSE, respResult.error().message);
  }
  const Response &resp = respResult.value();
  if (resp.isError()) {
    return Error(E_SERVER_ERROR, resp.payload);
  }
  return resp.payload;
}

// BeaconServer API - Block operations (binary T_REQ_BLOCK_GET)

Client::Roe<Ledger::ChainNode> Client::fetchBlock(uint64_t blockId) {
  log().debug << "Requesting block " << blockId;

  std::string payload = utl::binaryPack(blockId);
  auto result = sendRequest(T_REQ_BLOCK_GET, payload);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  Ledger::ChainNode node;
  if (!node.ltsFromString(result.value())) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize block");
  }
  return node;
}

Client::Roe<Client::AccountInfo> Client::fetchAccountInfo(const uint64_t accountId) {
  log().debug << "Requesting account info for account " << accountId;

  std::string payload = utl::binaryPack(accountId);
  auto result = sendRequest(T_REQ_ACCOUNT_GET, payload);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  AccountInfo info;
  if (!info.ltsFromString(result.value())) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize account info");
  }
  return info;
}

Client::Roe<Client::BeaconState> Client::registerMinerServer(const network::TcpEndpoint &endpoint) {
  log().debug << "Registering miner server: " << endpoint;

  std::string payload = endpoint.ltsToString();
  auto result = sendRequest(T_REQ_REGISTER, payload);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  try {
    nlohmann::json response = nlohmann::json::parse(result.value());
    BeaconState state;
    auto parseResult = state.ltsFromJson(response);
    if (!parseResult) {
      return Error(parseResult.error().code, parseResult.error().message);
    }
    return state;
  } catch (const std::exception& e) {
    return Error(E_PARSE_ERROR, std::string("Failed to parse BeaconState JSON: ") + e.what());
  }
}

Client::Roe<Client::BeaconState> Client::fetchBeaconState() {
  log().debug << "Requesting beacon state (checkpoint, block)";

  auto result = sendRequest(T_REQ_STATUS, "");
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  try {
    nlohmann::json response = nlohmann::json::parse(result.value());
    BeaconState state;
    auto parseResult = state.ltsFromJson(response);
    if (!parseResult) {
      return Error(parseResult.error().code, parseResult.error().message);
    }
    return state;
  } catch (const std::exception& e) {
    return Error(E_PARSE_ERROR, std::string("Failed to parse BeaconState JSON: ") + e.what());
  }
}

Client::Roe<bool> Client::addBlock(const Ledger::ChainNode& block) {
  log().debug << "Adding block " << block.block.index;

  std::string payload = block.ltsToString();
  auto result = sendRequest(T_REQ_BLOCK_ADD, payload);
  if (!result) {
    return result.error();
  }
  return true;
}

// MinerServer API - Transaction operations

Client::Roe<void> Client::addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  log().debug << "Adding transaction";

  std::string payload = utl::binaryPack(signedTx);
  auto result = sendRequest(T_REQ_TRANSACTION_ADD, payload);
  if (!result) {
    return result.error();
  }

  return {};
}

// MinerServer API - Status

Client::Roe<Client::MinerStatus> Client::fetchMinerStatus() {
  log().debug << "Requesting miner status";

  auto result = sendRequest(T_REQ_STATUS, "");
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  try {
    nlohmann::json response = nlohmann::json::parse(result.value());
    MinerStatus status;
    auto parseResult = status.ltsFromJson(response);
    if (!parseResult) {
      return Error(parseResult.error().code, parseResult.error().message);
    }
    return status;
  } catch (const std::exception& e) {
    return Error(E_PARSE_ERROR, std::string("Failed to parse MinerStatus JSON: ") + e.what());
  }
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
