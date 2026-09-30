#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "lib/common/Service.h"
#include "../network/ServerAmpSupport.h"
#include "RequestQueue.h"
#include "../network/LedgerRpcProtocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace pp {

/**
 * Base for role servers. The role's runLoop thread is the server thread: it
 * owns role state and handles RPC requests one at a time between duties via
 * serveRequestsFor(). See docs/architecture/THREADING.md.
 */
class Server : public Service {
public:
  Server();
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

  /** Server thread only. */
  virtual std::string handleParsedRequest(const Client::Request& request) = 0;

  /**
   * Server thread: handle queued requests until `budget` elapses (or stop is
   * set). Role runLoops call this in place of sleeping between duties.
   */
  void serveRequestsFor(std::chrono::milliseconds budget);

  /** Io lane: queue for the server thread, or reply busy when full / stopped. */
  void enqueueRequest(std::string body, RequestQueue::Reply reply);

  /** Before start only (tests): queue capacity and max time a request may wait. */
  void setRequestLimits(size_t capacity, std::chrono::milliseconds maxWait);

  Service::Roe<void> startAmpServer(const network::LedgerAmpConfig& config);
  void stopAmpServer();
  bool isAmpServerRunning() const { return ampSupport_ && ampSupport_->isRunning(); }

  void onStop() override;

private:
  /** Queued requests beyond this are refused with a busy reply. */
  static constexpr size_t kRequestQueueCapacity = 1024;

  std::string handleRequest(const std::string& request);
  /** Refuse new requests and reply to pending ones; runs before AMP stops. */
  void closeRequestQueue();

  std::string workDir_;
  std::unique_ptr<network::ServerAmpSupport> ampSupport_;
  size_t requestCapacity_{kRequestQueueCapacity};
  /** A request older than a client's read timeout was given up on: reply without doing the work. */
  std::chrono::milliseconds maxRequestWait_{ledger::rpc::kDefaultReadTimeout};
  std::unique_ptr<RequestQueue> requests_;
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
