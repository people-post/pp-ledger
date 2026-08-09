#include "Client.h"
#include "lib/common/Logger.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Serialize.hpp"
#include "lib/common/Utilities.h"

#include <sstream>

namespace pp {


std::ostream& operator<<(std::ostream& os, const Client::Request& req) {
  os << "Request{version=" << req.version << ", type=" << req.type << ", payload=" << req.payload.size() << " bytes}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const Client::Wallet& wallet) {
  os << "Wallet{balances: {";
  bool first = true;
  for (const auto& [tokenId, balance] : wallet.mBalances) {
    if (!first) os << ", ";
    os << tokenId << ": " << balance;
    first = false;
  }
  os << "}, publicKeys: [";
  bool firstKey = true;
  for (const auto& pk : wallet.publicKeys) {
    if (!firstKey) os << ", ";
    os << utl::toJsonSafeString(pk);
    firstKey = false;
  }
  os << "], minSignatures: " << (int)wallet.minSignatures
     << ", keyType: " << (int)wallet.keyType << "}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const Client::UserAccount& account) {
  os << "UserAccount{wallet: " << account.wallet << ", meta: \"" << account.meta << "\"}";
  return os;
}

pp::common::Meta Client::Wallet::ltsToMeta() const {
  pp::common::Meta balances;
  for (const auto &[tokenId, balance] : mBalances) {
    balances.set(std::to_string(tokenId), static_cast<int64_t>(balance));
  }
  std::vector<pp::common::Meta::Value> keys;
  keys.reserve(publicKeys.size());
  for (const auto &pk : publicKeys) {
    keys.push_back(utl::toJsonSafeString(pk));
  }
  pp::common::Meta j;
  j.set("mBalances", balances);
  j.set("publicKeys", pp::common::Meta::array(std::move(keys)));
  j.set("minSignatures", static_cast<uint64_t>(minSignatures));
  j.set("keyType", static_cast<uint64_t>(keyType));
  return j;
}

std::string Client::UserAccount::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Client::UserAccount::ltsFromString(const std::string& str) {
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

pp::common::Meta Client::UserAccount::ltsToMeta() const {
  pp::common::Meta j;
  j.set("wallet", wallet.ltsToMeta());
  j.set("meta", utl::toJsonSafeString(meta));
  return j;
}

pp::common::Meta Client::MinerInfo::ltsToMeta() const {
  pp::common::Meta m;
  m.set("id", id);
  m.set("tLastMessage", tLastMessage);
  m.set("endpoint", endpoint);
  return m;
}

Client::Roe<bool> Client::MinerInfo::ltsFromMeta(const pp::common::Meta &meta) {
  id = meta.getOrDefault("id", uint64_t{0});
  tLastMessage = meta.getOrDefault("tLastMessage", int64_t{0});
  endpoint = meta.getOrDefault("endpoint", std::string{});
  return true;
}

pp::common::Meta Client::MinerStatus::ltsToMeta() const {
  pp::common::Meta m;
  m.set("minerId", minerId);
  m.set("stake", stake);
  m.set("nextBlockId", nextBlockId);
  m.set("currentSlot", currentSlot);
  m.set("currentEpoch", currentEpoch);
  m.set("pendingTransactions", pendingTransactions);
  m.set("nStakeholders", nStakeholders);
  m.set("isSlotLeader", isSlotLeader);
  return m;
}

Client::Roe<bool> Client::MinerStatus::ltsFromMeta(const pp::common::Meta &meta) {
  minerId = meta.getOrDefault("minerId", uint64_t{0});
  stake = meta.getOrDefault("stake", uint64_t{0});
  nextBlockId = meta.getOrDefault("nextBlockId", uint64_t{0});
  currentSlot = meta.getOrDefault("currentSlot", uint64_t{0});
  currentEpoch = meta.getOrDefault("currentEpoch", uint64_t{0});
  pendingTransactions = meta.getOrDefault("pendingTransactions", uint64_t{0});
  nStakeholders = meta.getOrDefault("nStakeholders", uint64_t{0});
  isSlotLeader = meta.getOrDefault("isSlotLeader", false);
  return true;
}

pp::common::Meta Client::BeaconState::ltsToMeta() const {
  pp::common::Meta m;
  m.set("currentTimestamp", currentTimestamp);
  m.set("checkpointId", checkpointId);
  m.set("nextBlockId", nextBlockId);
  m.set("currentSlot", currentSlot);
  m.set("currentEpoch", currentEpoch);
  m.set("nStakeholders", nStakeholders);
  return m;
}

Client::Roe<bool> Client::BeaconState::ltsFromMeta(const pp::common::Meta &meta) {
  currentTimestamp = meta.getOrDefault("currentTimestamp", int64_t{0});
  checkpointId = meta.getOrDefault("checkpointId", uint64_t{0});
  nextBlockId = meta.getOrDefault("nextBlockId", uint64_t{0});
  currentSlot = meta.getOrDefault("currentSlot", uint64_t{0});
  currentEpoch = meta.getOrDefault("currentEpoch", uint64_t{0});
  nStakeholders = meta.getOrDefault("nStakeholders", uint64_t{0});
  return true;
}

pp::common::Meta Client::TxGetByWalletResponse::ltsToMeta() const {
  std::vector<pp::common::Meta::Value> txs;
  txs.reserve(transactions.size());
  for (const auto &tx : transactions) {
    txs.push_back(std::make_shared<pp::common::Meta>(tx.ltsToMeta()));
  }
  pp::common::Meta j;
  j.set("transactions", pp::common::Meta::array(std::move(txs)));
  j.set("nextBlockId", nextBlockId);
  return j;
}

pp::common::Meta Client::CalibrationResponse::ltsToMeta() const {
  pp::common::Meta j;
  j.set("msTimestamp", msTimestamp);
  j.set("nextBlockId", nextBlockId);
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
  auto ep = network::IpEndpoint::ltsFromString(endpoint);
  if (ep.port == 0) {
    return Error(E_NOT_CONNECTED, "Invalid endpoint: " + endpoint);
  }
  endpoint_ = ep;
  return {};
}

void Client::setEndpoint(const network::IpEndpoint &endpoint) {
  endpoint_ = endpoint;
}

Client::Roe<std::string> Client::sendRequest(uint32_t type, const std::string &payload,
                                             std::chrono::milliseconds timeout) {
  if (endpoint_.port == 0) {
    return Error(E_NOT_CONNECTED, getErrorMessage(E_NOT_CONNECTED));
  }

  Request req;
  req.version = Request::VERSION;
  req.type = type;
  req.payload = payload;

  std::string requestData = utl::binaryPack(req);
  log().debug << "Sending binary request: " << req;

  auto result = fetchClient_.fetchSync(endpoint_, requestData, timeout);

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
  auto result = sendRequest(T_REQ_BLOCK_GET, payload, TIMEOUT_DATA);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  Ledger::ChainNode node;
  if (!node.ltsFromString(result.value())) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize block");
  }
  return node;
}

Client::Roe<Client::UserAccount> Client::fetchUserAccount(const uint64_t accountId) {
  log().debug << "Requesting user account: " << accountId;

  std::string payload = utl::binaryPack(accountId);
  auto result = sendRequest(T_REQ_ACCOUNT_GET, payload, TIMEOUT_DATA);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  UserAccount account;
  if (!account.ltsFromString(result.value())) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize user account");
  }
  return account;
}

Client::Roe<Client::BeaconState> Client::registerMinerServer(const MinerInfo &minerInfo) {
  log().debug << "Registering miner server: " << minerInfo.id << " " << minerInfo.endpoint;

  std::string payload = utl::binaryPack(minerInfo.ltsToMeta());
  auto result = sendRequest(T_REQ_REGISTER, payload, TIMEOUT_FAST);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto metaResult = utl::binaryUnpack<pp::common::Meta>(result.value());
  if (!metaResult) {
    return Error(E_INVALID_RESPONSE,
                 "Failed to unpack beacon state Meta: " + metaResult.error().message);
  }
  BeaconState state;
  auto parseResult = state.ltsFromMeta(metaResult.value());
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }
  return state;
}

Client::Roe<Client::BeaconState> Client::fetchBeaconState() {
  log().debug << "Requesting beacon state (checkpoint, block)";

  auto result = sendRequest(T_REQ_STATUS, "", TIMEOUT_FAST);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto metaResult = utl::binaryUnpack<pp::common::Meta>(result.value());
  if (!metaResult) {
    return Error(E_INVALID_RESPONSE,
                 "Failed to unpack beacon state Meta: " + metaResult.error().message);
  }
  BeaconState state;
  auto parseResult = state.ltsFromMeta(metaResult.value());
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }
  return state;
}

Client::Roe<Client::CalibrationResponse> Client::fetchCalibration() {
  log().debug << "Requesting precise timestamp for calibration";

  auto result = sendRequest(T_REQ_CALIBRATION, "", TIMEOUT_FAST);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto roe = utl::binaryUnpack<CalibrationResponse>(result.value());
  if (!roe) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack calibration response: " + roe.error().message);
  }
  return roe.value();
}

Client::Roe<std::vector<Client::MinerInfo>> Client::fetchMinerList() {
  log().debug << "Requesting miner list";

  auto result = sendRequest(T_REQ_MINER_LIST, "", TIMEOUT_FAST);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto listResult =
      utl::binaryUnpack<std::vector<pp::common::Meta>>(result.value());
  if (!listResult) {
    return Error(E_INVALID_RESPONSE,
                 "Failed to unpack miner list: " + listResult.error().message);
  }
  std::vector<MinerInfo> miners;
  miners.reserve(listResult.value().size());
  for (const auto &meta : listResult.value()) {
    MinerInfo info;
    auto parseResult = info.ltsFromMeta(meta);
    if (!parseResult) {
      return Error(parseResult.error().code, parseResult.error().message);
    }
    miners.push_back(std::move(info));
  }
  return miners;
}

Client::Roe<Client::TxGetByWalletResponse> Client::fetchTransactionsByWallet(const TxGetByWalletRequest &request) {
  log().debug << "Requesting transactions by wallet: " << request.walletId << " " << request.beforeBlockId;

  std::string payload = utl::binaryPack(request);
  auto result = sendRequest(T_REQ_TX_GET_BY_WALLET, payload, TIMEOUT_DATA);

  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto responseResult = utl::binaryUnpack<TxGetByWalletResponse>(result.value());
  if (!responseResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack transactions by wallet response: " + responseResult.error().message);
  }
  return responseResult.value();
}

Client::Roe<Ledger::Record>
Client::fetchTransactionByIndex(const TxGetByIndexRequest &request) {
  log().debug << "Requesting transaction by index: " << request.txIndex;

  std::string payload = utl::binaryPack(request);
  auto result = sendRequest(T_REQ_TX_GET_BY_INDEX, payload, TIMEOUT_DATA);

  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto responseResult = utl::binaryUnpack<Ledger::Record>(result.value());
  if (!responseResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack transaction by index response: " + responseResult.error().message);
  }
  return responseResult.value();
}

Client::Roe<bool> Client::addBlock(const Ledger::ChainNode& block) {
  log().debug << "Adding block " << block.block.index;

  std::string payload = block.ltsToString();
  auto result = sendRequest(T_REQ_BLOCK_ADD, payload, TIMEOUT_DATA);
  if (!result) {
    return result.error();
  }
  return true;
}

// MinerServer API - Transaction operations

Client::Roe<void> Client::addTransaction(const Ledger::Record &record) {
  log().debug << "Adding transaction";

  std::string payload = utl::binaryPack(record);
  auto result = sendRequest(T_REQ_TX_ADD, payload, TIMEOUT_DATA);
  if (!result) {
    return result.error();
  }

  return {};
}

// MinerServer API - Status

Client::Roe<Client::MinerStatus> Client::fetchMinerStatus() {
  log().debug << "Requesting miner status";

  auto result = sendRequest(T_REQ_STATUS, "", TIMEOUT_FAST);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto metaResult = utl::binaryUnpack<pp::common::Meta>(result.value());
  if (!metaResult) {
    return Error(E_INVALID_RESPONSE,
                 "Failed to unpack miner status Meta: " +
                     metaResult.error().message);
  }
  MinerStatus status;
  auto parseResult = status.ltsFromMeta(metaResult.value());
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }
  return status;
}

} // namespace pp
