#include "Beacon.h"
#include "../client/Client.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pp {

Beacon::Beacon() : network::FetchServer() {
  log().info << "Beacon initialized";
}

bool Beacon::start(const std::string &dataDir) {
  if (isRunning()) {
    log().warning << "Beacon is already running";
    return false;
  }

  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(dataDir) / "config.json";
  std::string configPathStr = configPath.string();

  log().info << "Starting beacon with work directory: " << dataDir;

  // Load configuration (includes port)
  auto configResult = loadConfig(configPathStr);
  if (!configResult) {
    log().error << "Failed to load configuration: "
                << configResult.error().message;
    return false;
  }

  // Start FetchServer with handler
  network::FetchServer::Config fetchServerConfig;
  fetchServerConfig.host = config_.network.host;
  fetchServerConfig.port = config_.network.port;
  fetchServerConfig.handler = [this](const std::string &request) {
    return handleServerRequest(request);
  };
  bool serverStarted = network::FetchServer::start(fetchServerConfig);

  if (!serverStarted) {
    log().error << "Failed to start FetchServer";
    return false;
  }

  // Connect to other beacon servers
  connectToOtherBeacons();

  return true;
}

Beacon::Roe<void> Beacon::loadConfig(const std::string &configPath) {
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

  // Parse JSON
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
      config_.network.port = config["port"].get<uint16_t>();
    } else {
      return Error(3, "Configuration file 'port' field is not an integer");
    }
  } else {
    config_.network.port = Client::DEFAULT_PORT;
  }

  // Load other beacon addresses (optional, can be empty)
  otherBeaconAddresses_.clear();
  if (config.contains("beacons") && config["beacons"].is_array()) {
    for (const auto &beaconAddr : config["beacons"]) {
      if (beaconAddr.is_string()) {
        std::string addr = beaconAddr.get<std::string>();
        otherBeaconAddresses_.push_back(addr);
        log().info << "Found beacon address in config: " << addr;
      }
    }
  }

  log().info << "Configuration loaded from " << configPath;
  log().info << "  Host: " << config_.network.host;
  log().info << "  Port: " << config_.network.port;
  log().info << "  Other beacons: " << otherBeaconAddresses_.size();
  return {};
}

std::string Beacon::handleServerRequest(const std::string &request) {
  log().debug << "Received request from server (" << request.size()
              << " bytes)";

  // Try to parse as JSON
  nlohmann::json reqJson;
  try {
    reqJson = nlohmann::json::parse(request);
  } catch (const nlohmann::json::parse_error &e) {
    log().error << "Failed to parse request JSON: " << e.what();
    nlohmann::json errorResp;
    errorResp["error"] = "invalid json";
    return errorResp.dump();
  }

  // Handle different request types
  if (reqJson.contains("type")) {
    std::string type = reqJson["type"].get<std::string>();

    if (type == "register") {
      // Server is registering itself
      if (reqJson.contains("address") && reqJson["address"].is_string()) {
        std::string serverAddress = reqJson["address"].get<std::string>();
        registerServer(serverAddress);
        log().info << "Registered server: " << serverAddress;

        nlohmann::json resp;
        resp["status"] = "ok";
        resp["message"] = "Server registered";
        return resp.dump();
      } else {
        nlohmann::json resp;
        resp["error"] = "missing address field";
        return resp.dump();
      }
    } else if (type == "heartbeat") {
      // Server is sending heartbeat
      if (reqJson.contains("address") && reqJson["address"].is_string()) {
        std::string serverAddress = reqJson["address"].get<std::string>();
        registerServer(serverAddress);
        log().debug << "Received heartbeat from: " << serverAddress;

        nlohmann::json resp;
        resp["status"] = "ok";
        return resp.dump();
      } else {
        nlohmann::json resp;
        resp["error"] = "missing address field";
        return resp.dump();
      }
    } else if (type == "query") {
      // Query active servers
      nlohmann::json resp;
      resp["status"] = "ok";
      resp["servers"] = nlohmann::json::array();

      std::vector<std::string> servers = getActiveServers();
      for (const auto &server : servers) {
        resp["servers"].push_back(server);
      }

      return resp.dump();
    } else {
      nlohmann::json resp;
      resp["error"] = "unknown request type: " + type;
      return resp.dump();
    }
  } else {
    nlohmann::json resp;
    resp["error"] = "missing type field";
    return resp.dump();
  }
}

void Beacon::registerServer(const std::string &serverAddress) {
  std::lock_guard<std::mutex> lock(serversMutex_);

  // Get current timestamp
  int64_t now = std::chrono::system_clock::now()
                    .time_since_epoch()
                    .count();

  // Update or add server
  activeServers_[serverAddress] = now;
  log().debug << "Updated server record: " << serverAddress;
}

std::vector<std::string> Beacon::getActiveServers() const {
  std::lock_guard<std::mutex> lock(serversMutex_);

  std::vector<std::string> servers;
  for (const auto &pair : activeServers_) {
    servers.push_back(pair.first);
  }
  return servers;
}

size_t Beacon::getActiveServerCount() const {
  std::lock_guard<std::mutex> lock(serversMutex_);
  return activeServers_.size();
}

void Beacon::connectToOtherBeacons() {
  if (otherBeaconAddresses_.empty()) {
    log().info << "No other beacon addresses configured";
    return;
  }

  log().info << "Connecting to " << otherBeaconAddresses_.size()
             << " other beacon servers";

  // For now, just log the addresses
  // In a full implementation, we might want to establish connections
  // to share server lists between beacons
  for (const auto &beaconAddr : otherBeaconAddresses_) {
    log().info << "  Beacon: " << beaconAddr;
  }
}

} // namespace pp
