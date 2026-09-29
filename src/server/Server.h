#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "lib/common/Service.h"
#include "../network/ServerAmpSupport.h"
#include "../network/ServerConfig.h"
#include "common/WorkerPool.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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

  std::string listenMultiaddr() const;
  network::LedgerAmpRuntime* ampRuntime();
  pp::amp::PeerLinkManager* peerLinks();

  static std::string packResponse(const std::string& payload);
  static std::string packResponse(uint16_t errorCode, const std::string& message);

  std::string dispatchUnframedRequest(const std::string& requestBody);
  virtual std::string handleParsedRequest(const Client::Request& request) = 0;

  Service::Roe<void> startAmpServer(const network::LedgerAmpConfig& config,
                                    network::ServerAmpSupport::PeerAllowed peer_allowed = {});
  void stopAmpServer();
  bool isAmpServerRunning() const { return ampSupport_ && ampSupport_->isRunning(); }

  void setPerformanceConfig(const network::PerformanceConfig& config) {
    performanceConfig_ = config;
  }

  void onStop() override;

  /**
   * Serializes RPC handlers and runLoop over server state. Lock order: any
   * per-client network mutex first, then this; never hold it across an RPC.
   */
  mutable std::mutex stateMutex_;

private:
  std::string handleRequest(const std::string& request);
  void startRequestHandlers();
  void stopRequestHandlers();

  std::string workDir_;
  network::PerformanceConfig performanceConfig_{};
  std::unique_ptr<network::ServerAmpSupport> ampSupport_;
  std::unique_ptr<WorkerPool> handlerPool_;
  std::atomic<bool> handlerStop_{false};
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
