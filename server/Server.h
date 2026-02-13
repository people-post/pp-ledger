#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "../lib/Service.h"
#include "../lib/ThreadSafeQueue.hpp"
#include "../network/FetchServer.h"
#include <cstdint>
#include <string>

namespace pp {

/**
 * Server - Base class for RelayServer, MinerServer, and BeaconServer.
 *
 * Provides common run(workDir) behavior: work directory setup, optional
 * signature file for directory recognition, log file handler, then
 * Service::run() (onStart + runLoop). Also provides shared request queue,
 * processQueuedRequest, and handleRequest(string) with virtual
 * handleParsedRequest and sendResponse for derived implementations.
 */
class Server : public Service {
public:
  Server() = default;
  ~Server() override = default;

  virtual Service::Roe<void> run(const std::string &workDir);

protected:
  virtual bool useSignatureFile() const { return true; }

  const std::string &getWorkDir() const { return workDir_; }
  virtual std::string getSignatureFileName() const = 0;
  virtual std::string getLogFileName() const = 0;
  virtual std::string getServerName() const = 0;
  virtual int32_t getRunErrorCode() const { return -1; }
  virtual network::TcpEndpoint getFetchServerEndpoint() const {
    return fetchServer_.getEndpoint();
  }

  static std::string packResponse(const std::string &payload);
  static std::string packResponse(uint16_t errorCode,
                                  const std::string &message);

  bool pollAndProcessOneRequest();

  virtual std::string handleParsedRequest(const Client::Request &request) = 0;

  Service::Roe<void> startFetchServer(const network::TcpEndpoint &endpoint);
  void stopFetchServer();

  /** Override to customize FetchServer config (e.g. whitelist) before start. */
  virtual void customizeFetchServerConfig(network::FetchServer::Config &config) {}

  void onStop() override;

private:
  struct QueuedRequest {
    int fd{-1};
    std::string request;
  };

  size_t getRequestQueueSize() const;
  void processQueuedRequest(QueuedRequest &qr);
  std::string handleRequest(const std::string &request);

  void sendResponse(int fd, const std::string &response);

  std::string workDir_;
  ThreadSafeQueue<QueuedRequest> requestQueue_;
  network::FetchServer fetchServer_;
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
