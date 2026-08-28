#pragma once

#include "BulkWriter.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Service.h"
#include "TcpServer.h"
#include "TcpConnection.h"
#include "Types.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace pp {
namespace network {

class IoMultiplexer;

class FetchServer : public Service {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  using RequestHandler =
      std::function<void(int fd, const std::string&, const IpEndpoint& endpoint)>;

  struct Config {
    IpEndpoint endpoint;
    RequestHandler handler{nullptr};
    std::vector<std::string> whitelist;
  };

  FetchServer();
  ~FetchServer() override;

  IpEndpoint getEndpoint() const { return server_.getEndpoint(); }
  Roe<void> addResponse(int fd, const std::string& response);
  Service::Roe<void> start(const Config& config);

protected:
  void runLoop() override;
  Service::Roe<void> onStart() override;
  void onStop() override;

private:
  struct ActiveConnection {
    int fd;
    IpEndpoint endpoint;
    enum class Stage { ReadLen, ReadBody };
    Stage stage{Stage::ReadLen};
    uint32_t expectedLen{0};
    std::string buffer;
  };

  Roe<IpEndpoint> getPeerEndpoint(int fd);
  bool isAllowedByWhitelist(const IpEndpoint& peer) const;
  void processReadEvents(const std::vector<int>& readyFds);
  void readFromConnection(ActiveConnection& conn);
  void closeAndRemoveConnection(ActiveConnection& conn, const std::string& reason);
  void dispatchCompleteFrameAndRemove(ActiveConnection& conn, std::string requestBody);
  bool tryParseSingleFrame(ActiveConnection& conn);
  void pollActiveReads();
  bool registerClientFd(int clientFd);
  void acceptPendingConnections();

  TcpServer server_;
  Config config_;
  BulkWriter writer_;
  std::unique_ptr<IoMultiplexer> ioMux_;
  std::map<int, ActiveConnection> activeConnections_;
};

} // namespace network
} // namespace pp
