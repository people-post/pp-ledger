#include "Client.h"
#include "../network/FetchClient.h"
#include "../lib/BinaryPack.h"
#include "../lib/Logger.h"

namespace pp {

Client::Client() : Module("client"), connected_(false) {
}

Client::~Client() {
    disconnect();
}

bool Client::init(const std::string& address, int port) {
    auto& logger = logging::getLogger("client");
    
    address_ = address;
    port_ = port;
    connected_ = true;
    
    logger.info << "Client initialized, target server: " << address << ":" << port;
    return true;
}

void Client::disconnect() {
    if (connected_) {
        auto& logger = logging::getLogger("client");
        logger.info << "Client disconnected";
        connected_ = false;
    }
}

bool Client::isConnected() const {
    return connected_;
}

Client::Roe<Client::Response> Client::sendRequest(const Request& request) {
    if (!connected_) {
        return Error(E_INVALID_REQUEST, "Client not connected");
    }
    
    auto& logger = logging::getLogger("client");
    
    // Serialize request
    std::string requestData = utl::binaryPack(request);
    
    // Send request using FetchClient
    network::FetchClient fetchClient;
    auto result = fetchClient.fetchSync(address_, static_cast<uint16_t>(port_), requestData);
    
    if (!result.isOk()) {
        logger.error << "Failed to send request: " << result.error().message;
        return Error(E_INVALID_RESPONSE, "Failed to send request: " + result.error().message);
    }
    
    // Deserialize response
    try {
        Response response = utl::binaryUnpack<Response>(result.value());
        
        if (response.version != VERSION) {
            return Error(E_VERSION, "Version mismatch");
        }
        
        return response;
    } catch (const std::exception& e) {
        logger.error << "Failed to deserialize response: " << e.what();
        return Error(E_INVALID_RESPONSE, "Failed to deserialize response");
    }
}

Client::Roe<Client::RespWalletInfo> Client::getWalletInfo(const std::string& walletId) {
    auto& logger = logging::getLogger("client");
    logger.debug << "Requesting wallet info for: " << walletId;
    
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
    
    const Response& response = result.value();
    if (response.errorCode != 0) {
        return Error(response.errorCode, "Server error: " + std::to_string(response.errorCode));
    }
    
    // Deserialize response data
    try {
        RespWalletInfo respData = utl::binaryUnpack<RespWalletInfo>(response.data);
        logger.debug << "Wallet info received: balance=" << respData.balance;
        return respData;
    } catch (const std::exception& e) {
        return Error(E_INVALID_DATA, "Failed to deserialize wallet info");
    }
}

Client::Roe<Client::RespAddTransaction> Client::addTransaction(const std::string& transaction) {
    auto& logger = logging::getLogger("client");
    logger.debug << "Submitting transaction";
    
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
    
    const Response& response = result.value();
    if (response.errorCode != 0) {
        return Error(response.errorCode, "Server error: " + std::to_string(response.errorCode));
    }
    
    // Deserialize response data
    try {
        RespAddTransaction respData = utl::binaryUnpack<RespAddTransaction>(response.data);
        logger.debug << "Transaction submitted successfully";
        return respData;
    } catch (const std::exception& e) {
        return Error(E_INVALID_DATA, "Failed to deserialize transaction response");
    }
}

Client::Roe<Client::RespValidators> Client::getValidators() {
    auto& logger = logging::getLogger("client");
    logger.debug << "Requesting validators";
    
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
    
    const Response& response = result.value();
    if (response.errorCode != 0) {
        return Error(response.errorCode, "Server error: " + std::to_string(response.errorCode));
    }
    
    // Deserialize response data
    try {
        RespValidators respData = utl::binaryUnpack<RespValidators>(response.data);
        logger.debug << "Validators received";
        return respData;
    } catch (const std::exception& e) {
        return Error(E_INVALID_DATA, "Failed to deserialize validators response");
    }
}

Client::Roe<Client::RespBlocks> Client::getBlocks(uint64_t fromIndex, uint64_t count) {
    auto& logger = logging::getLogger("client");
    logger.debug << "Requesting blocks from index " << fromIndex << ", count=" << count;
    
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
    
    const Response& response = result.value();
    if (response.errorCode != 0) {
        return Error(response.errorCode, "Server error: " + std::to_string(response.errorCode));
    }
    
    // Deserialize response data
    try {
        RespBlocks respData = utl::binaryUnpack<RespBlocks>(response.data);
        logger.debug << "Received " << respData.blocks.size() << " blocks";
        return respData;
    } catch (const std::exception& e) {
        return Error(E_INVALID_DATA, "Failed to deserialize blocks response");
    }
}

} // namespace pp
