#include "FetchServer.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

namespace pp {
namespace network {

namespace {

void unregisterRead(int queueFd, int fd) {
#ifdef __APPLE__
  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  kevent(queueFd, &ev, 1, nullptr, 0, nullptr);
#else
  epoll_ctl(queueFd, EPOLL_CTL_DEL, fd, nullptr);
#endif
}

} // namespace

FetchServer::FetchServer() {}

FetchServer::~FetchServer() {
#ifdef __APPLE__
  if (kqueueFd_ >= 0) {
    ::close(kqueueFd_);
  }
#else
  if (epollFd_ >= 0) {
    ::close(epollFd_);
  }
#endif
  // Close any remaining connections
  for (auto& pair : activeConnections_) {
    ::close(pair.first);
  }
}

FetchServer::Roe<void> FetchServer::addResponse(int fd, const std::string &response) {
  if (response.size() > TcpConnection::MAX_FRAME_SIZE) {
    return Error(-3, "Response frame too large: " + std::to_string(response.size()));
  }

  std::string framed;
  framed.resize(sizeof(uint32_t) + response.size());
  uint32_t netLen = htonl(static_cast<uint32_t>(response.size()));
  std::memcpy(framed.data(), &netLen, sizeof(netLen));
  if (!response.empty()) {
    std::memcpy(framed.data() + sizeof(uint32_t), response.data(), response.size());
  }

  auto result = writer_.add(fd, framed);
  if (!result) {
    return Error(-3, "Failed to add response to bulk writer: " + result.error().message);
  }
  return {};
}

Service::Roe<void> FetchServer::start(const Config &config) {
  config_ = config;

  log().info << "Starting server on " << config_.endpoint.address << ":"
             << config_.endpoint.port;

  // Call base class start() which will call onStart() then spawn thread
  return Service::start();
}

Service::Roe<void> FetchServer::onStart() {
  // Create epoll/kqueue instance for monitoring connections
#ifdef __APPLE__
  kqueueFd_ = kqueue();
  if (kqueueFd_ < 0) {
    return Service::Error(-1, "Failed to create kqueue: " + std::string(std::strerror(errno)));
  }
#else
  epollFd_ = epoll_create1(0);
  if (epollFd_ < 0) {
    return Service::Error(-1, "Failed to create epoll: " + std::string(std::strerror(errno)));
  }
#endif

  // Start listening
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
  
  // Clean up epoll/kqueue
#ifdef __APPLE__
  if (kqueueFd_ >= 0) {
    ::close(kqueueFd_);
    kqueueFd_ = -1;
  }
#else
  if (epollFd_ >= 0) {
    ::close(epollFd_);
    epollFd_ = -1;
  }
#endif
  
  // Close all active connections
  for (auto& pair : activeConnections_) {
    ::close(pair.first);
  }
  activeConnections_.clear();
}

bool FetchServer::setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return false;
  }
  return true;
}

FetchServer::Roe<IpEndpoint> FetchServer::getPeerEndpoint(int fd) {
  struct sockaddr_in peer_addr;
  socklen_t addr_len = sizeof(peer_addr);
  if (getpeername(fd, (struct sockaddr *)&peer_addr, &addr_len) != 0) {
    return Error(static_cast<int32_t>(errno),
                 "getpeername failed: " + std::string(std::strerror(errno)));
  }
  IpEndpoint peer;
  char addr_str[INET_ADDRSTRLEN];
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
      continue; // Connection already removed
    }
    
    readFromConnection(it->second);
    
    // Check if connection is complete (will be marked by readFromConnection)
    // We'll detect this by checking if recv returned 0 or error
  }
}

void FetchServer::readFromConnection(ActiveConnection& conn) {
  char buffer[8192];
  
  while (true) {
    ssize_t bytesRead = ::recv(conn.fd, buffer, sizeof(buffer), 0);
    
    if (bytesRead > 0) {
      // Data received, append to buffer
      conn.buffer.append(buffer, bytesRead);
      
      // Try to parse a single framed request (one-request-per-connection).
      if (tryParseSingleFrame(conn)) {
        return;
      }
    } else if (bytesRead == 0) {
      // Peer closed before completing a full frame (or after sending).
      closeAndRemoveConnection(conn,
                               "Connection closed by peer while reading request from " +
                                   conn.endpoint.address + ":" +
                                   std::to_string(conn.endpoint.port) + " (fd=" +
                                   std::to_string(conn.fd) + ")");
      break;
      
    } else {
      // Error or would block
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No more data available right now
        break;
      } else {
        // Real error
        log().error << "Error reading from fd " << conn.fd << ": " << std::strerror(errno);
        closeAndRemoveConnection(conn, "");
        break;
      }
    }
  }
}

void FetchServer::closeAndRemoveConnection(ActiveConnection& conn, const std::string& reason) {
  if (!reason.empty()) {
    log().error << reason;
  }
  unregisterRead(
#ifdef __APPLE__
      kqueueFd_,
#else
      epollFd_,
#endif
      conn.fd);
  ::close(conn.fd);
  int fd = conn.fd;
  activeConnections_.erase(fd);
}

void FetchServer::dispatchCompleteFrameAndRemove(ActiveConnection& conn, std::string requestBody) {
  log().info << "Received complete request from " << conn.endpoint.address
             << ":" << conn.endpoint.port << " (" << requestBody.size()
             << " bytes, fd=" << conn.fd << ")";

  // Stop reading; keep fd open for response (BulkWriter closes after send).
  unregisterRead(
#ifdef __APPLE__
      kqueueFd_,
#else
      epollFd_,
#endif
      conn.fd);

  try {
    if (config_.handler) {
      config_.handler(conn.fd, requestBody, conn.endpoint);
    }
    log().debug << "Request processed successfully for fd " << conn.fd;
  } catch (const std::exception &e) {
    log().error << "Error processing request: " << e.what();
    ::close(conn.fd);
  }

  int fd = conn.fd;
  activeConnections_.erase(fd);
}

bool FetchServer::tryParseSingleFrame(ActiveConnection& conn) {
  // Returns true if a complete frame was parsed and dispatched (and conn removed).
  while (true) {
    if (conn.stage == ActiveConnection::Stage::ReadLen) {
      if (conn.buffer.size() < sizeof(uint32_t)) {
        return false;
      }

      uint32_t netLen = 0;
      std::memcpy(&netLen, conn.buffer.data(), sizeof(netLen));
      uint32_t len = ntohl(netLen);
      if (len > TcpConnection::MAX_FRAME_SIZE) {
        log().error << "Frame too large from " << conn.endpoint.address
                    << ":" << conn.endpoint.port << " (" << len
                    << " bytes, fd=" << conn.fd << ")";
        closeAndRemoveConnection(conn, "");
        return true;
      }
      conn.expectedLen = len;
      conn.stage = ActiveConnection::Stage::ReadBody;
      conn.buffer.erase(0, sizeof(uint32_t));
      continue;
    }

    // Stage::ReadBody
    if (conn.buffer.size() < conn.expectedLen) {
      return false;
    }

    std::string requestBody = conn.buffer.substr(0, conn.expectedLen);
    dispatchCompleteFrameAndRemove(conn, std::move(requestBody));
    return true;
  }
}

void FetchServer::pollActiveReads() {
  if (activeConnections_.empty()) {
    return;
  }
#ifdef __APPLE__
  struct kevent events[32];
  struct timespec timeout = {0, 1000000}; // 1ms
  int n = kevent(kqueueFd_, nullptr, 0, events, 32, &timeout);
  if (n > 0) {
    std::vector<int> readyFds;
    readyFds.reserve(n);
    for (int i = 0; i < n; ++i) {
      readyFds.push_back(static_cast<int>(events[i].ident));
    }
    processReadEvents(readyFds);
  }
#else
  struct epoll_event events[32];
  int n = epoll_wait(epollFd_, events, 32, 1); // 1ms timeout
  if (n > 0) {
    std::vector<int> readyFds;
    readyFds.reserve(n);
    for (int i = 0; i < n; ++i) {
      readyFds.push_back(events[i].data.fd);
    }
    processReadEvents(readyFds);
  }
#endif
}

bool FetchServer::registerClientFd(int clientFd) {
#ifdef __APPLE__
  struct kevent ev;
  EV_SET(&ev, clientFd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
  if (kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr) < 0) {
    log().error << "Failed to add fd to kqueue: " << std::strerror(errno);
    return false;
  }
#else
  struct epoll_event ev = {};
  ev.events = EPOLLIN | EPOLLET; // Edge-triggered for efficiency
  ev.data.fd = clientFd;
  if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
    log().error << "Failed to add fd to epoll: " << std::strerror(errno);
    return false;
  }
#endif
  return true;
}

void FetchServer::acceptPendingConnections() {
  while (true) {
    auto acceptResult = server_.accept();
    if (!acceptResult) {
      break;
    }

    int clientFd = acceptResult.value();

    auto peerResult = getPeerEndpoint(clientFd);
    if (!peerResult) {
      log().error << "Failed to get peer endpoint for fd " << clientFd << ": "
                  << peerResult.error().message;
      ::close(clientFd);
      continue;
    }
    IpEndpoint peerEndpoint = peerResult.value();

    if (!isAllowedByWhitelist(peerEndpoint)) {
      log().info << "Rejected connection from " << peerEndpoint.address << ":"
                 << peerEndpoint.port << " (not in whitelist)";
      ::close(clientFd);
      continue;
    }

    if (!setNonBlocking(clientFd)) {
      log().error << "Failed to set non-blocking mode for fd " << clientFd;
      ::close(clientFd);
      continue;
    }

    if (!registerClientFd(clientFd)) {
      ::close(clientFd);
      continue;
    }

    ActiveConnection conn;
    conn.fd = clientFd;
    conn.endpoint = peerEndpoint;
    activeConnections_[clientFd] = std::move(conn);

    log().debug << "Accepted new connection from " << peerEndpoint.address
                << ":" << peerEndpoint.port << " (fd=" << clientFd << ")";
  }
}

void FetchServer::runLoop() {
  log().debug << "Server loop started";

  while (!isStopSet()) {
    // Wait for events on the listening socket
    auto waitResult = server_.waitForEvents(100); // 100ms timeout
    if (!waitResult) {
      // Timeout - check for ready connections
      pollActiveReads();
      continue;
    }

    // Accept all pending connections (edge-triggered accept must be drained).
    acceptPendingConnections();

    // Service any reads that became ready.
    pollActiveReads();
  }

  log().debug << "Server loop ended";
}

} // namespace network
} // namespace pp
