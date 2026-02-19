#ifndef PP_LEDGER_RELAY_SERVER_H
#define PP_LEDGER_RELAY_SERVER_H

#include "../client/Client.h"
#include "../lib/ResultOrError.hpp"
#include "../network/TcpConnection.h"
#include "../network/Types.hpp"
#include "Relay.h"
#include "Server.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace pp {

class RelayServer : public Server {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Error codes
  static constexpr const int32_t E_CONFIG = -1;
  static constexpr const int32_t E_NETWORK = -2;
  static constexpr const int32_t E_RELAY = -3;
  static constexpr const int32_t E_REQUEST = -4;

  RelayServer();
  ~RelayServer() = default;

  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

protected:
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "RelayServer"; }
  int32_t getRunErrorCode() const override { return E_RELAY; }

  /**
   * Service thread main loop - processes queued requests
   */
  void runLoop() override;

  /**
   * Called before service thread starts - initializes beacon and network
   */
  Service::Roe<void> onStart() override;

  /**
   * Called after service thread stops - cleans up resources
   */
  void onStop() override;

private:
  constexpr static const char *FILE_CONFIG = "config.json";
  constexpr static const char *FILE_LOG = "relay.log";
  constexpr static const char *FILE_SIGNATURE = ".signature";
  constexpr static const char *DIR_DATA = "data";

  struct RunFileConfig {
    std::string host{Client::DEFAULT_HOST};
    uint16_t port{Client::DEFAULT_BEACON_PORT};
    std::string beacon;

    nlohmann::json ltsToJson();
    Roe<void> ltsFromJson(const nlohmann::json &jd);
  };

  struct NetworkConfig {
    network::TcpEndpoint endpoint;
    std::string beacon;
  };

  struct Config {
    NetworkConfig network;
  };

  void initHandlers();
  Roe<void> syncBlocksFromBeacon();
  /** Periodically sync to latest block from beacon since last sync. */
  void syncBlocksPeriodically();

  void registerServer(const Client::MinerInfo &minerInfo);
  Client::BeaconState buildStateResponse() const;

  std::string handleParsedRequest(const Client::Request &request) override;

  // Getters
  Roe<std::string> hBlockGet(const Client::Request &request);
  Roe<std::string> hAccountGet(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hMinerList(const Client::Request &request);
  Roe<std::string> hUnsupported(const Client::Request &request);

  // Modifiers
  Roe<std::string> hBlockAdd(const Client::Request &request);
  Roe<std::string> hRegister(const Client::Request &request);

  Config config_;
  Relay relay_;
  Client client_;

  static constexpr std::chrono::seconds BLOCK_SYNC_INTERVAL{5};
  std::chrono::steady_clock::time_point lastBlockSyncTime_{};

  using Handler =
      std::function<Roe<std::string>(const Client::Request &request)>;
  std::map<uint32_t, Handler> requestHandlers_;

  std::map<uint64_t, Client::MinerInfo> mMiners_;
};

} // namespace pp

#endif // PP_LEDGER_RELAY_SERVER_H
