#include "Server.h"
#include "../client/Client.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <filesystem>

namespace pp {

Service::Roe<void> Server::run(const std::string& workDir) {
  workDir_ = workDir;

  if (useSignatureFile()) {
    std::filesystem::path signaturePath = std::filesystem::path(workDir) / getFileSignature();
    if (!std::filesystem::exists(workDir)) {
      std::filesystem::create_directories(workDir);
      auto result = utl::writeToNewFile(signaturePath.string(), "");
      if (!result) {
        return Service::Error(getRunErrorCode(),
                             "Failed to create signature file: " + result.error().message);
      }
    }
    if (!std::filesystem::exists(signaturePath)) {
      return Service::Error(getRunErrorCode(),
                            "Work directory not recognized, please remove it manually and try again");
    }
  }

  log().info << "Running " << getServerName() << " with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + getFileLog(), logging::getLevel());

  return Service::run();
}

std::string Server::packResponse(const std::string& payload) {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = 0;
  resp.payload = payload;
  return utl::binaryPack(resp);
}

std::string Server::packResponse(uint16_t errorCode, const std::string& message) {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = errorCode;
  resp.payload = message;
  return utl::binaryPack(resp);
}

} // namespace pp
