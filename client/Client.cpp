#include "Client.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../network/FetchClient.h"

namespace pp {

std::string Client::getErrorMessage(uint16_t errorCode) {
  switch (errorCode) {
  case E_VERSION:
    return "Protocol version mismatch. Please update your client to match the "
           "server version.";
  case E_INVALID_REQUEST:
    return "Invalid request format. The request could not be understood by the "
           "server.";
  case E_INVALID_RESPONSE:
    return "Invalid response from server. The server response could not be "
           "processed.";
  case E_INVALID_DATA:
    return "Invalid data format. The data provided is malformed or cannot be "
           "parsed.";
  case E_INVALID_SIGNATURE:
    return "Invalid cryptographic signature. The signature verification "
           "failed.";
  case E_INVALID_TIMESTAMP:
    return "Invalid timestamp. The timestamp is outside the acceptable range.";
  case E_INVALID_NONCE:
    return "Invalid nonce value. The nonce does not match the expected value.";
  case E_INVALID_HASH:
    return "Invalid hash value. The hash does not match the expected value.";
  case E_INVALID_BLOCK:
    return "Invalid block. The block data is corrupted or does not meet "
           "validation requirements.";
  case E_INVALID_TRANSACTION:
    return "Invalid transaction. The transaction data is malformed or violates "
           "protocol rules.";
  case E_INVALID_VALIDATOR:
    return "Invalid validator. The validator information is incorrect or not "
           "authorized.";
  case E_INVALID_BLOCKCHAIN:
    return "Invalid blockchain state. The blockchain data is corrupted or "
           "inconsistent.";
  case E_INVALID_LEDGER:
    return "Invalid ledger state. The ledger data is corrupted or "
           "inconsistent.";
  case E_INVALID_WALLET:
    return "Wallet not found. The specified wallet does not exist in the "
           "system.";
  case E_INVALID_ADDRESS:
    return "Invalid address format. The address provided is not in the correct "
           "format.";
  default:
    return "Unknown error occurred. Error code: " + std::to_string(errorCode);
  }
}

Client::Client() : Module("client"), connected_(false) {}

Client::~Client() { disconnect(); }

bool Client::init(const std::string &address, int port) {
  address_ = address;
  port_ = port;
  connected_ = true;

  log().info << "Client initialized, target server: " << address << ":"
              << port;
  return true;
}

void Client::disconnect() {
  if (connected_) {
    log().info << "Client disconnected";
    connected_ = false;
  }
}

bool Client::isConnected() const { return connected_; }

Client::Roe<Client::Response> Client::sendRequest(const Request &request) {
  if (!connected_) {
    return Error(E_INVALID_REQUEST,
                 getErrorMessage(E_INVALID_REQUEST) +
                     " Client is not connected to the server.");
  }

  // Serialize request
  std::string requestData = utl::binaryPack(request);

  // Send request using FetchClient
  network::FetchClient fetchClient;
  auto result = fetchClient.fetchSync(address_, static_cast<uint16_t>(port_),
                                      requestData);

  if (!result.isOk()) {
    log().error << "Failed to send request: " << result.error().message;
    return Error(E_INVALID_RESPONSE, getErrorMessage(E_INVALID_RESPONSE) +
                                         " Details: " + result.error().message);
  }

  // Deserialize response
  auto responseResult = utl::binaryUnpack<Response>(result.value());
  if (!responseResult) {
    log().error << "Failed to deserialize response: "
                 << responseResult.error().message;
    return Error(E_INVALID_RESPONSE,
                 getErrorMessage(E_INVALID_RESPONSE) +
                     " Details: " + responseResult.error().message);
  }

  Response response = responseResult.value();
  if (response.version != VERSION) {
    return Error(E_VERSION, getErrorMessage(E_VERSION));
  }

  return response;
}

Client::Roe<Client::RespInfo> Client::getInfo() {
  log().debug << "Requesting server info";

  // Create request
  Request request;
  request.version = VERSION;
  request.type = T_REQ_INFO;
  request.data = ""; // No data needed for info request

  // Send request
  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const Response &response = result.value();
  if (response.errorCode != 0) {
    std::string friendlyMsg = getErrorMessage(response.errorCode);
    return Error(response.errorCode, friendlyMsg);
  }

  // Deserialize response data
  auto respDataResult = utl::binaryUnpack<RespInfo>(response.data);
  if (!respDataResult) {
    return Error(E_INVALID_DATA,
                 getErrorMessage(E_INVALID_DATA) +
                     " Details: " + respDataResult.error().message);
  }

  log().debug << "Server info received: blocks="
               << respDataResult.value().blockCount
               << ", slot=" << respDataResult.value().currentSlot
               << ", epoch=" << respDataResult.value().currentEpoch;
  return respDataResult.value();
}

Client::Roe<Client::RespWalletInfo>
Client::getWalletInfo(const std::string &walletId) {
  log().debug << "Requesting wallet info for: " << walletId;

  // Create request data
  ReqWalletInfo reqData;
  reqData.walletId = walletId;

  // Create request
  Request request;
  request.version = VERSION;
  request.type = T_REQ_QUERY_WALLET;
  request.data = utl::binaryPack(reqData);

  // Send request
  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const Response &response = result.value();
  if (response.errorCode != 0) {
    std::string friendlyMsg = getErrorMessage(response.errorCode);
    return Error(response.errorCode, friendlyMsg);
  }

  // Deserialize response data
  auto respDataResult = utl::binaryUnpack<RespWalletInfo>(response.data);
  if (!respDataResult) {
    return Error(E_INVALID_DATA,
                 getErrorMessage(E_INVALID_DATA) +
                     " Details: " + respDataResult.error().message);
  }

  log().debug << "Wallet info received: balance="
               << respDataResult.value().balance;
  return respDataResult.value();
}

Client::Roe<Client::RespAddTransaction>
Client::addTransaction(const std::string &transaction) {
  log().debug << "Submitting transaction";

  // Create request data
  ReqAddTransaction reqData;
  reqData.transaction = transaction;

  // Create request
  Request request;
  request.version = VERSION;
  request.type = T_REQ_ADD_TRANSACTION;
  request.data = utl::binaryPack(reqData);

  // Send request
  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const Response &response = result.value();
  if (response.errorCode != 0) {
    std::string friendlyMsg = getErrorMessage(response.errorCode);
    return Error(response.errorCode, friendlyMsg);
  }

  // Deserialize response data
  auto respDataResult = utl::binaryUnpack<RespAddTransaction>(response.data);
  if (!respDataResult) {
    return Error(E_INVALID_DATA,
                 getErrorMessage(E_INVALID_DATA) +
                     " Details: " + respDataResult.error().message);
  }

  log().debug << "Transaction submitted successfully";
  return respDataResult.value();
}

Client::Roe<Client::RespValidators> Client::getValidators() {
  log().debug << "Requesting validators";

  // Create request data
  ReqValidators reqData;
  reqData.validators = "";

  // Create request
  Request request;
  request.version = VERSION;
  request.type = T_REQ_BEACON_VALIDATORS;
  request.data = utl::binaryPack(reqData);

  // Send request
  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const Response &response = result.value();
  if (response.errorCode != 0) {
    std::string friendlyMsg = getErrorMessage(response.errorCode);
    return Error(response.errorCode, friendlyMsg);
  }

  // Deserialize response data
  auto respDataResult = utl::binaryUnpack<RespValidators>(response.data);
  if (!respDataResult) {
    return Error(E_INVALID_DATA,
                 getErrorMessage(E_INVALID_DATA) +
                     " Details: " + respDataResult.error().message);
  }

  log().debug << "Validators received";
  return respDataResult.value();
}

Client::Roe<Client::RespBlocks> Client::getBlocks(uint64_t fromIndex,
                                                  uint64_t count) {
  log().debug << "Requesting blocks from index " << fromIndex
               << ", count=" << count;

  // Create request data
  ReqBlocks reqData;
  reqData.fromIndex = fromIndex;
  reqData.count = count;

  // Create request
  Request request;
  request.version = VERSION;
  request.type = T_REQ_BLOCKS;
  request.data = utl::binaryPack(reqData);

  // Send request
  auto result = sendRequest(request);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  const Response &response = result.value();
  if (response.errorCode != 0) {
    std::string friendlyMsg = getErrorMessage(response.errorCode);
    return Error(response.errorCode, friendlyMsg);
  }

  // Deserialize response data
  auto respDataResult = utl::binaryUnpack<RespBlocks>(response.data);
  if (!respDataResult) {
    return Error(E_INVALID_DATA,
                 getErrorMessage(E_INVALID_DATA) +
                     " Details: " + respDataResult.error().message);
  }

  log().debug << "Received " << respDataResult.value().blocks.size()
               << " blocks";
  return respDataResult.value();
}

} // namespace pp
