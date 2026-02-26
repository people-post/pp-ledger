#include "DhtRunner.h"
#include "dht/dht.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sodium.h>

namespace pp {
namespace network {

namespace {

constexpr int IPv4_COMPACT_BYTES = 6;   // 4 addr + 2 port
constexpr int IPv6_COMPACT_BYTES = 18;  // 16 addr + 2 port

void setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/** Parse "host:port" into host and port; return false if invalid. */
bool parseEndpoint(const std::string& endpoint, std::string& host, uint16_t& port) {
  size_t colon = endpoint.find(':');
  if (colon == std::string::npos || colon == 0)
    return false;
  host = endpoint.substr(0, colon);
  std::string portStr = endpoint.substr(colon + 1);
  if (portStr.empty())
    return false;
  try {
    unsigned long p = std::stoul(portStr);
    if (p > 65535)
      return false;
    port = static_cast<uint16_t>(p);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace

DhtRunner::NodeId DhtRunner::getDefaultNetworkId() {
  NodeId id{};
  const char s[] = "pp-ledger-dht-v1";
  constexpr size_t n = sizeof(s) - 1;
  for (size_t i = 0; i < 20; ++i)
    id[i] = (i < n) ? static_cast<unsigned char>(s[i]) : 0;
  return id;
}

DhtRunner::DhtRunner() {
  redirectLogger("DhtRunner");
}

DhtRunner::~DhtRunner() {
  stop();
}

DhtRunner::Roe<void> DhtRunner::start(const Config& config) {
  if (running_) {
    return Error("DhtRunner already running");
  }
  config_ = config;

  if (config_.networkId == NodeId{}) {
    return Error("config.networkId must be non-zero");
  }
  if (config_.myTcpPort == 0) {
    return Error("config.myTcpPort must be non-zero");
  }

  {
    Roe<void> er = loadOrCreateNodeId(config_.nodeIdPath, nodeId_);
    if (!er)
      return er;
  }

  int fd4 = -1, fd6 = -1;
  {
    Roe<void> er = createSockets(config_.dhtPort, fd4, fd6);
    if (!er)
      return er;
  }
  socket4_ = fd4;
  socket6_ = fd6;

  const unsigned char* version = nullptr;
  if (config_.dhtVersion.size() >= 4) {
    version = reinterpret_cast<const unsigned char*>(config_.dhtVersion.data());
  }
  if (dht_init(socket4_, socket6_, nodeId_.data(), version) < 0) {
    if (socket4_ >= 0) close(socket4_);
    if (socket6_ >= 0) close(socket6_);
    socket4_ = socket6_ = -1;
    return Error(std::string("dht_init failed: ") + std::strerror(errno));
  }

  doBootstrap(config_.bootstrapEndpoints);

  stopRequested_ = false;
  running_ = true;
  thread_ = std::thread(&DhtRunner::runLoop, this);

  log().info << "DhtRunner started (UDP port " << config_.dhtPort
             << ", TCP announce " << config_.myTcpPort << ")";
  return {};
}

void DhtRunner::stop() {
  if (!running_)
    return;
  stopRequested_ = true;
  if (thread_.joinable())
    thread_.join();
  running_ = false;
  dht_uninit();
  if (socket4_ >= 0) {
    close(socket4_);
    socket4_ = -1;
  }
  if (socket6_ >= 0) {
    close(socket6_);
    socket6_ = -1;
  }
  log().info << "DhtRunner stopped";
}

std::vector<IpEndpoint> DhtRunner::getDiscoveredPeers() const {
  std::lock_guard<std::mutex> lock(peersMutex_);
  return discoveredPeers_;
}

DhtRunner::Roe<void> DhtRunner::createSockets(uint16_t port, int& fd4, int& fd6) {
  fd4 = -1;
  fd6 = -1;
  std::string portStr = port != 0 ? std::to_string(port) : "0";
  int one = 1;

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* res4 = nullptr;
  int r = getaddrinfo(nullptr, portStr.c_str(), &hints, &res4);
  if (r != 0) {
    return Error(std::string("getaddrinfo IPv4: ") + gai_strerror(r));
  }
  fd4 = socket(res4->ai_family, res4->ai_socktype, res4->ai_protocol);
  if (fd4 < 0) {
    freeaddrinfo(res4);
    return Error(std::string("socket IPv4: ") + std::strerror(errno));
  }
  if (setsockopt(fd4, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    close(fd4);
    freeaddrinfo(res4);
    return Error(std::string("setsockopt IPv4: ") + std::strerror(errno));
  }
  if (bind(fd4, res4->ai_addr, res4->ai_addrlen) < 0) {
    close(fd4);
    freeaddrinfo(res4);
    return Error(std::string("bind IPv4: ") + std::strerror(errno));
  }
  freeaddrinfo(res4);
  setNonBlocking(fd4);

  hints.ai_family = AF_INET6;
  struct addrinfo* res6 = nullptr;
  r = getaddrinfo(nullptr, portStr.c_str(), &hints, &res6);
  if (r != 0) {
    close(fd4);
    return Error(std::string("getaddrinfo IPv6: ") + gai_strerror(r));
  }
  fd6 = socket(res6->ai_family, res6->ai_socktype, res6->ai_protocol);
  if (fd6 >= 0) {
    setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd6, res6->ai_addr, res6->ai_addrlen) < 0) {
      close(fd6);
      fd6 = -1;
    } else {
      setNonBlocking(fd6);
    }
  }
  freeaddrinfo(res6);
  return {};
}

DhtRunner::Roe<void> DhtRunner::loadOrCreateNodeId(const std::string& path,
                                                    NodeId& out) {
  if (!path.empty()) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
      unsigned char buf[20];
      size_t n = fread(buf, 1, 20, f);
      fclose(f);
      if (n == 20) {
        memcpy(out.data(), buf, 20);
        return {};
      }
    }
  }
  randombytes_buf(out.data(), 20);
  if (!path.empty()) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
      fwrite(out.data(), 1, 20, f);
      fclose(f);
    }
  }
  return {};
}

void DhtRunner::doBootstrap(const std::vector<std::string>& endpoints) {
  for (const std::string& ep : endpoints) {
    std::string host;
    uint16_t port = 0;
    if (!parseEndpoint(ep, host, port))
      continue;
    std::string portStr = std::to_string(port);
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    int r = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (r != 0)
      continue;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
      if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
        if (dht_ping_node(p->ai_addr, static_cast<int>(p->ai_addrlen)) >= 0) {
          log().debug << "DHT bootstrap ping: " << ep;
        }
      }
    }
    freeaddrinfo(res);
  }
}

void DhtRunner::onDhtEvent(int event, const unsigned char* info_hash,
                            const void* data, size_t data_len) {
  (void)info_hash;
  if (event != DHT_EVENT_VALUES && event != DHT_EVENT_VALUES6)
    return;
  int bytesPerNode = (event == DHT_EVENT_VALUES) ? IPv4_COMPACT_BYTES
                                                  : IPv6_COMPACT_BYTES;
  if (data_len == 0 || data_len % bytesPerNode != 0)
    return;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  std::vector<IpEndpoint> added;
  while (data_len >= static_cast<size_t>(bytesPerNode)) {
    IpEndpoint ep;
    if (event == DHT_EVENT_VALUES) {
      char addr[INET_ADDRSTRLEN];
      struct in_addr ia;
      memcpy(&ia, p, 4);
      inet_ntop(AF_INET, &ia, addr, sizeof(addr));
      ep.address = addr;
      ep.port = (static_cast<uint16_t>(p[4]) << 8) | p[5];
    } else {
      char addr[INET6_ADDRSTRLEN];
      struct in6_addr ia6;
      memcpy(&ia6, p, 16);
      inet_ntop(AF_INET6, &ia6, addr, sizeof(addr));
      ep.address = addr;
      ep.port = (static_cast<uint16_t>(p[16]) << 8) | p[17];
    }
    added.push_back(ep);
    p += bytesPerNode;
    data_len -= bytesPerNode;
  }
  if (!added.empty()) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    for (const IpEndpoint& e : added) {
      auto it = std::find_if(discoveredPeers_.begin(), discoveredPeers_.end(),
                             [&e](const IpEndpoint& x) {
                               return x.address == e.address && x.port == e.port;
                             });
      if (it == discoveredPeers_.end())
        discoveredPeers_.push_back(e);
    }
    log().debug << "DHT discovered " << added.size() << " peer(s), total "
                << discoveredPeers_.size();
  }
}

void DhtRunner::dhtCallback(void* closure, int event,
                            const unsigned char* info_hash,
                            const void* data, size_t data_len) {
  auto* self = static_cast<DhtRunner*>(closure);
  self->onDhtEvent(event, info_hash, data, data_len);
}

void DhtRunner::runLoop() {
  time_t tosleep = 1;
  unsigned char buf[4096];
  struct sockaddr_storage from = {};
  socklen_t fromlen = sizeof(from);

  if (socket4_ >= 0) {
    dht_search(config_.networkId.data(), static_cast<int>(config_.myTcpPort),
               AF_INET, &dhtCallback, this);
  }
  if (socket6_ >= 0) {
    dht_search(config_.networkId.data(), static_cast<int>(config_.myTcpPort),
               AF_INET6, &dhtCallback, this);
  }

  while (!stopRequested_) {
    struct pollfd pfd[2] = {};
    int nfds = 0;
    if (socket4_ >= 0) {
      pfd[nfds].fd = socket4_;
      pfd[nfds].events = POLLIN;
      nfds++;
    }
    if (socket6_ >= 0) {
      pfd[nfds].fd = socket6_;
      pfd[nfds].events = POLLIN;
      nfds++;
    }
    int timeout_ms = static_cast<int>(tosleep) * 1000;
    if (timeout_ms < 100)
      timeout_ms = 100;
    if (timeout_ms > 60000)
      timeout_ms = 60000;

    int pr = (nfds > 0) ? poll(pfd, nfds, timeout_ms) : 0;
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      log().error << "DHT poll: " << std::strerror(errno);
      tosleep = 1;
      continue;
    }

    bool hadData = false;
    for (int i = 0; i < nfds; i++) {
      if (!(pfd[i].revents & POLLIN))
        continue;
      fromlen = sizeof(from);
      ssize_t n = recvfrom(pfd[i].fd, buf, sizeof(buf), 0,
                           reinterpret_cast<struct sockaddr*>(&from), &fromlen);
      if (n <= 0)
        continue;
      hadData = true;
      time_t ts = tosleep;
      dht_periodic(buf, static_cast<size_t>(n),
                   reinterpret_cast<struct sockaddr*>(&from),
                   static_cast<int>(fromlen),
                   &ts, &dhtCallback, this);
      tosleep = ts;
    }
    if (!hadData) {
      time_t ts = tosleep;
      dht_periodic(nullptr, 0, nullptr, 0, &ts, &dhtCallback, this);
      tosleep = ts;
    }
  }
}

} // namespace network
} // namespace pp
