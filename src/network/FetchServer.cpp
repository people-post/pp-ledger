#include "FetchServer.h"
#include "LedgerFrameCodec.h"
#include "platform/IoMultiplexer.h"
#include "platform/NetworkPlatform.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace pp {
namespace network {

FetchServer::FetchServer() = default;

FetchServer::~FetchServer() {
  for (auto& pair : activeConnections_) {
    socketClose(pair.first);
  }
}

FetchServer::Roe<void> FetchServer::addResponse(int fd, const std::string& response) {
  auto framed = LedgerFrameCodec::encode(response);
  if (!framed) {
    return Error(-3, framed.error().message);
  }

  auto result = writer_.add(fd, framed.value());
  if (!result) {
    return Error(-3, "Failed to add response to bulk writer: " + result.error().message);
  }
  return {};
}

Service::Roe<void> FetchServer::start(const Config& config) {
  config_ = config;

  log().info << "Starting server on " << config_.endpoint.address << ":"
             << config_.endpoint.port;

  return Service::start();
}

Service::Roe<void> FetchServer::onStart() {
  ioMux_ = IoMultiplexer::create();
  if (!ioMux_) {
    return Service::Error(-1, "Failed to create I/O multiplexer");
  }

  auto listenResult = server_.listen(config_.endpoint);
  if (!listenResult) {
    return Service::Error(-2, "Failed to start listening: " + listenResult.error().message);
  }

  auto startResult = writer_.start();
  if (!startResult) {
    return Service::Error(-3, "Failed to start writer: " + startResult.error().message);
  }
  return {};
}

void FetchServer::onStop() {
  writer_.stop();
  server_.stop();
  ioMux_.reset();

  for (auto& pair : activeConnections_) {
    socketClose(pair.first);
  }
  activeConnections_.clear();
}

FetchServer::Roe<IpEndpoint> FetchServer::getPeerEndpoint(int fd) {
  sockaddr_in peer_addr {};
  socklen_t addr_len = sizeof(peer_addr);
  if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len) != 0) {
    return Error(static_cast<int32_t>(socketLastError()),
                 "getpeername failed: " + socketErrorString(socketLastError()));
  }
  IpEndpoint peer;
  char addr_str[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &peer_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
  peer.address = addr_str;
  peer.port = ntohs(peer_addr.sin_port);
  return peer;
}

bool FetchServer::isAllowedByWhitelist(const IpEndpoint& peer) const {
  if (config_.whitelist.empty()) {
    return true;
  }
  return std::find(config_.whitelist.begin(), config_.whitelist.end(),
                   peer.address) != config_.whitelist.end();
}

void FetchServer::processReadEvents(const std::vector<int>& readyFds) {
  for (int fd : readyFds) {
    auto it = activeConnections_.find(fd);
    if (it == activeConnections_.end()) {
      continue;
    }
    readFromConnection(it->second);
  }
}

void FetchServer::readFromConnection(ActiveConnection& conn) {
  char buffer[8192];

  while (true) {
#if defined(_WIN32)
    const int bytesRead =
        ::recv(static_cast<SOCKET>(conn.fd), buffer, static_cast<int>(sizeof(buffer)), 0);
#else
    const ssize_t bytesRead = ::recv(conn.fd, buffer, sizeof(buffer), 0);
#endif

    if (bytesRead > 0) {
      conn.buffer.append(buffer, static_cast<size_t>(bytesRead));
      if (tryParseSingleFrame(conn)) {
        return;
      }
    } else if (bytesRead == 0) {
      closeAndRemoveConnection(
          conn, "Connection closed by peer while reading request from " +
                    conn.endpoint.address + ":" + std::to_string(conn.endpoint.port) +
                    " (fd=" + std::to_string(conn.fd) + ")");
      break;
    } else {
      const int err = socketLastError();
      if (socketWouldBlock(err)) {
        break;
      }
      log().error << "Error reading from fd " << conn.fd << ": "
                  << socketErrorString(err);
      closeAndRemoveConnection(conn, "");
      break;
    }
  }
}

void FetchServer::closeAndRemoveConnection(ActiveConnection& conn,
                                           const std::string& reason) {
  if (!reason.empty()) {
    log().error << reason;
  }
  if (ioMux_) {
    ioMux_->remove(conn.fd);
  }
  socketClose(conn.fd);
  const int fd = conn.fd;
  activeConnections_.erase(fd);
}

void FetchServer::dispatchCompleteFrameAndRemove(ActiveConnection& conn,
                                                 std::string requestBody) {
  log().info << "Received complete request from " << conn.endpoint.address << ":"
             << conn.endpoint.port << " (" << requestBody.size()
             << " bytes, fd=" << conn.fd << ")";

  if (ioMux_) {
    ioMux_->remove(conn.fd);
  }

  try {
    if (config_.handler) {
      config_.handler(conn.fd, requestBody, conn.endpoint);
    }
    log().debug << "Request processed successfully for fd " << conn.fd;
  } catch (const std::exception& e) {
    log().error << "Error processing request: " << e.what();
    socketClose(conn.fd);
  }

  const int fd = conn.fd;
  activeConnections_.erase(fd);
}

bool FetchServer::tryParseSingleFrame(ActiveConnection& conn) {
  while (true) {
    if (conn.stage == ActiveConnection::Stage::ReadLen) {
      if (conn.buffer.size() < sizeof(uint32_t)) {
        return false;
      }

      uint32_t netLen = 0;
      std::memcpy(&netLen, conn.buffer.data(), sizeof(netLen));
      auto lenResult = LedgerFrameCodec::decodeLengthPrefix(&netLen, sizeof(netLen));
      if (!lenResult) {
        log().error << lenResult.error().message << " from " << conn.endpoint.address
                    << ":" << conn.endpoint.port << " (fd=" << conn.fd << ")";
        closeAndRemoveConnection(conn, "");
        return true;
      }
      conn.expectedLen = lenResult.value();
      conn.stage = ActiveConnection::Stage::ReadBody;
      conn.buffer.erase(0, sizeof(uint32_t));
      continue;
    }

    if (conn.buffer.size() < conn.expectedLen) {
      return false;
    }

    std::string requestBody = conn.buffer.substr(0, conn.expectedLen);
    dispatchCompleteFrameAndRemove(conn, std::move(requestBody));
    return true;
  }
}

void FetchServer::pollActiveReads() {
  if (activeConnections_.empty() || !ioMux_) {
    return;
  }

  std::vector<IoMultiplexer::ReadyEvent> ready;
  const int n = ioMux_->wait(1, ready);
  if (n > 0) {
    std::vector<int> readyFds;
    readyFds.reserve(ready.size());
    for (const auto& ev : ready) {
      if (ev.events & IoMultiplexer::Readable) {
        readyFds.push_back(ev.fd);
      }
    }
    processReadEvents(readyFds);
  }
}

bool FetchServer::registerClientFd(int clientFd) {
  if (!ioMux_) {
    return false;
  }
  if (!socketSetNonBlocking(clientFd)) {
    log().error << "Failed to set non-blocking mode for fd " << clientFd;
    return false;
  }
  if (!ioMux_->add(clientFd, IoMultiplexer::Readable, true)) {
    log().error << "Failed to add fd to I/O multiplexer: "
                << socketErrorString(socketLastError());
    return false;
  }
  return true;
}

void FetchServer::acceptPendingConnections() {
  while (true) {
    auto acceptResult = server_.accept();
    if (!acceptResult) {
      break;
    }

    const int clientFd = acceptResult.value();

    auto peerResult = getPeerEndpoint(clientFd);
    if (!peerResult) {
      log().error << "Failed to get peer endpoint for fd " << clientFd << ": "
                  << peerResult.error().message;
      socketClose(clientFd);
      continue;
    }
    const IpEndpoint peerEndpoint = peerResult.value();

    if (!isAllowedByWhitelist(peerEndpoint)) {
      log().info << "Rejected connection from " << peerEndpoint.address << ":"
                 << peerEndpoint.port << " (not in whitelist)";
      socketClose(clientFd);
      continue;
    }

    if (!registerClientFd(clientFd)) {
      socketClose(clientFd);
      continue;
    }

    ActiveConnection conn;
    conn.fd = clientFd;
    conn.endpoint = peerEndpoint;
    activeConnections_[clientFd] = std::move(conn);

    log().debug << "Accepted new connection from " << peerEndpoint.address << ":"
                << peerEndpoint.port << " (fd=" << clientFd << ")";
  }
}

void FetchServer::runLoop() {
  log().debug << "Server loop started";

  while (!isStopSet()) {
    auto waitResult = server_.waitForEvents(100);
    if (!waitResult) {
      pollActiveReads();
      continue;
    }

    acceptPendingConnections();
    pollActiveReads();
  }

  log().debug << "Server loop ended";
}

} // namespace network
} // namespace pp
