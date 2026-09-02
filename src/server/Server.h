#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "lib/common/Service.h"
#include "lib/common/ThreadSafeQueue.hpp"
#include "../network/FetchServer.h"
#include "common/WorkerPool.h"

#ifdef PP_LEDGER_HAS_AMP
#include "../network/ServerAmpSupport.h"
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace pp {

class Server : public Service {
public:
  Server() = default;
  ~Server() override;

  virtual Service::Roe<void> run(const std::string& workDir);

protected:
  virtual bool useSignatureFile() const { return true; }

  static Roe<void> ensureWorkDirectory(const std::string& workDir,
                                       const std::string& signatureFileName,
                                       int32_t errorCode = -1);

  const std::string& getWorkDir() const { return workDir_; }
  virtual std::string getSignatureFileName() const = 0;
  virtual std::string getLogFileName() const = 0;
  virtual std::string getServerName() const = 0;
  virtual int32_t getRunErrorCode() const { return -1; }
  virtual network::IpEndpoint getFetchServerEndpoint() const {
    return fetchServer_.getEndpoint();
  }

  size_t getRequestQueueSize() const;

  static std::string packResponse(const std::string& payload);
  static std::string packResponse(uint16_t errorCode, const std::string& message);

  bool pollAndProcessOneRequest();
  size_t pollAndProcessAllRequests(size_t maxCount = 100);

  std::string dispatchUnframedRequest(const std::string& requestBody);
  virtual std::string handleParsedRequest(const Client::Request& request) = 0;

  Service::Roe<void> startFetchServer(const network::IpEndpoint& endpoint);
  void stopFetchServer();

#ifdef PP_LEDGER_HAS_AMP
  Service::Roe<void> startAmpServer(const network::LedgerAmpConfig& config);
  void stopAmpServer();
  bool isAmpServerRunning() const { return ampSupport_ && ampSupport_->isRunning(); }
#endif

  virtual void customizeFetchServerConfig(network::FetchServer::Config& /*config*/) {}

  void setPerformanceConfig(const network::PerformanceConfig& config) {
    performanceConfig_ = config;
  }

  void onStop() override;

private:
  struct QueuedRequest {
    int fd{-1};
    std::string request;
  };

  void processQueuedRequest(QueuedRequest& qr);
  std::string handleRequest(const std::string& request);
  void sendResponse(int fd, const std::string& response);
  void startRequestHandlers();
  void stopRequestHandlers();
  void handlerWorkerLoop();

  std::string workDir_;
  network::PerformanceConfig performanceConfig_{};
  ThreadSafeQueue<QueuedRequest> requestQueue_;
  network::FetchServer fetchServer_;
#ifdef PP_LEDGER_HAS_AMP
  std::unique_ptr<network::ServerAmpSupport> ampSupport_;
#endif
  std::unique_ptr<WorkerPool> handlerPool_;
  std::atomic<bool> handlerStop_{false};
  size_t handlerWorkerCount_{0};
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
