#include "DhtRunner.h"
#include "dht/dht.h"
#include "platform/NetworkPlatform.h"
#include "platform/PollWait.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sodium.h>

namespace pp {
namespace network {

namespace {

constexpr int IPv4_COMPACT_BYTES = 6;
constexpr int IPv6_COMPACT_BYTES = 18;

bool parseEndpoint(const std::string& endpoint, std::string& host, uint16_t& port) {
  const size_t colon = endpoint.find(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  host = endpoint.substr(0, colon);
  const std::string portStr = endpoint.substr(colon + 1);
  if (portStr.empty()) {
    return false;
  }
  try {
    const unsigned long p = std::stoul(portStr);
    if (p > 65535) {
      return false;
    }
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
  for (size_t i = 0; i < 20; ++i) {
    id[i] = (i < n) ? static_cast<unsigned char>(s[i]) : 0;
  }
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
  if (!networkPlatformInit()) {
    return Error("Failed to initialize network platform");
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
    if (!er) {
      return er;
    }
  }

  int fd4 = -1;
  int fd6 = -1;
  {
    Roe<void> er = createSockets(config_.dhtPort, fd4, fd6);
    if (!er) {
      return er;
    }
  }
  socket4_ = fd4;
  socket6_ = fd6;

  const unsigned char* version = nullptr;
  if (config_.dhtVersion.size() >= 4) {
    version = reinterpret_cast<const unsigned char*>(config_.dhtVersion.data());
  }
  if (dht_init(socket4_, socket6_, nodeId_.data(), version) < 0) {
    if (socket4_ >= 0) {
      socketClose(socket4_);
    }
    if (socket6_ >= 0) {
      socketClose(socket6_);
    }
    socket4_ = socket6_ = -1;
    return Error(std::string("dht_init failed: ") + socketErrorString(socketLastError()));
  }

  doBootstrap(config_.bootstrapEndpoints);

  stopRequested_ = false;
  running_ = true;
  thread_ = std::thread(&DhtRunner::runLoop, this);

  log().info << "DhtRunner started (UDP port " << config_.dhtPort << ", TCP announce "
             << config_.myTcpPort << ")";
  return {};
}

void DhtRunner::stop() {
  if (!running_) {
    return;
  }
  stopRequested_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  running_ = false;
  dht_uninit();
  if (socket4_ >= 0) {
    socketClose(socket4_);
    socket4_ = -1;
  }
  if (socket6_ >= 0) {
    socketClose(socket6_);
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
  const std::string portStr = port != 0 ? std::to_string(port) : "0";
  int one = 1;

  addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* res4 = nullptr;
  int r = getaddrinfo(nullptr, portStr.c_str(), &hints, &res4);
  if (r != 0) {
    return Error(std::string("getaddrinfo IPv4: ") + gai_strerror(r));
  }
  fd4 = static_cast<int>(::socket(res4->ai_family, res4->ai_socktype, res4->ai_protocol));
  if (fd4 < 0) {
    freeaddrinfo(res4);
    return Error(std::string("socket IPv4: ") + socketErrorString(socketLastError()));
  }
  if (setsockopt(fd4, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                 sizeof(one)) < 0) {
    socketClose(fd4);
    freeaddrinfo(res4);
    return Error(std::string("setsockopt IPv4: ") + socketErrorString(socketLastError()));
  }
  if (bind(fd4, res4->ai_addr, static_cast<int>(res4->ai_addrlen)) < 0) {
    socketClose(fd4);
    freeaddrinfo(res4);
    return Error(std::string("bind IPv4: ") + socketErrorString(socketLastError()));
  }
  freeaddrinfo(res4);
  socketSetNonBlocking(fd4);

  hints.ai_family = AF_INET6;
  addrinfo* res6 = nullptr;
  r = getaddrinfo(nullptr, portStr.c_str(), &hints, &res6);
  if (r != 0) {
    socketClose(fd4);
    return Error(std::string("getaddrinfo IPv6: ") + gai_strerror(r));
  }
  fd6 = static_cast<int>(::socket(res6->ai_family, res6->ai_socktype, res6->ai_protocol));
  if (fd6 >= 0) {
    setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));
    if (bind(fd6, res6->ai_addr, static_cast<int>(res6->ai_addrlen)) < 0) {
      socketClose(fd6);
      fd6 = -1;
    } else {
      socketSetNonBlocking(fd6);
    }
  }
  freeaddrinfo(res6);
  return {};
}

DhtRunner::Roe<void> DhtRunner::loadOrCreateNodeId(const std::string& path, NodeId& out) {
  if (!path.empty()) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
      unsigned char buf[20];
      const size_t n = fread(buf, 1, 20, f);
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
    if (!parseEndpoint(ep, host, port)) {
      continue;
    }
    const std::string portStr = std::to_string(port);
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const int r = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (r != 0) {
      continue;
    }
    for (addrinfo* p = res; p; p = p->ai_next) {
      if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
        if (dht_ping_node(p->ai_addr, static_cast<int>(p->ai_addrlen)) >= 0) {
          log().debug << "DHT bootstrap ping: " << ep;
        }
      }
    }
    freeaddrinfo(res);
  }
}

void DhtRunner::onDhtEvent(int event, const unsigned char* info_hash, const void* data,
                           size_t data_len) {
  (void)info_hash;
  if (event != DHT_EVENT_VALUES && event != DHT_EVENT_VALUES6) {
    return;
  }
  const int bytesPerNode =
      (event == DHT_EVENT_VALUES) ? IPv4_COMPACT_BYTES : IPv6_COMPACT_BYTES;
  if (data_len == 0 || data_len % static_cast<size_t>(bytesPerNode) != 0) {
    return;
  }
  const unsigned char* p = static_cast<const unsigned char*>(data);
  std::vector<IpEndpoint> added;
  while (data_len >= static_cast<size_t>(bytesPerNode)) {
    IpEndpoint ep;
    if (event == DHT_EVENT_VALUES) {
      char addr[INET_ADDRSTRLEN] = {};
      in_addr ia {};
      memcpy(&ia, p, 4);
      inet_ntop(AF_INET, &ia, addr, sizeof(addr));
      ep.address = addr;
      ep.port = (static_cast<uint16_t>(p[4]) << 8) | p[5];
    } else {
      char addr[INET6_ADDRSTRLEN] = {};
      in6_addr ia6 {};
      memcpy(&ia6, p, 16);
      inet_ntop(AF_INET6, &ia6, addr, sizeof(addr));
      ep.address = addr;
      ep.port = (static_cast<uint16_t>(p[16]) << 8) | p[17];
    }
    added.push_back(ep);
    p += bytesPerNode;
    data_len -= static_cast<size_t>(bytesPerNode);
  }
  if (!added.empty()) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    for (const IpEndpoint& e : added) {
      const auto it = std::find_if(discoveredPeers_.begin(), discoveredPeers_.end(),
                                   [&e](const IpEndpoint& x) {
                                     return x.address == e.address && x.port == e.port;
                                   });
      if (it == discoveredPeers_.end()) {
        discoveredPeers_.push_back(e);
      }
    }
    log().debug << "DHT discovered " << added.size() << " peer(s), total "
                << discoveredPeers_.size();
  }
}

void DhtRunner::dhtCallback(void* closure, int event, const unsigned char* info_hash,
                            const void* data, size_t data_len) {
  auto* self = static_cast<DhtRunner*>(closure);
  self->onDhtEvent(event, info_hash, data, data_len);
}

void DhtRunner::runLoop() {
  time_t tosleep = 1;
  unsigned char buf[4096];
  sockaddr_storage from {};
  socklen_t fromlen = sizeof(from);

  if (socket4_ >= 0) {
    dht_search(config_.networkId.data(), static_cast<int>(config_.myTcpPort), AF_INET,
               &dhtCallback, this);
  }
  if (socket6_ >= 0) {
    dht_search(config_.networkId.data(), static_cast<int>(config_.myTcpPort), AF_INET6,
               &dhtCallback, this);
  }

  while (!stopRequested_) {
    std::vector<PollWaitEntry> entries;
    if (socket4_ >= 0) {
      PollWaitEntry entry {};
      entry.fd = socket4_;
      entry.events = POLLIN;
      entries.push_back(entry);
    }
    if (socket6_ >= 0) {
      PollWaitEntry entry {};
      entry.fd = socket6_;
      entry.events = POLLIN;
      entries.push_back(entry);
    }

    int timeout_ms = static_cast<int>(tosleep) * 1000;
    if (timeout_ms < 100) {
      timeout_ms = 100;
    }
    if (timeout_ms > 60000) {
      timeout_ms = 60000;
    }

    const int pr = entries.empty() ? 0 : pollWait(entries, timeout_ms);
    if (pr < 0) {
      if (socketInterrupted(socketLastError())) {
        continue;
      }
      log().error << "DHT poll: " << socketErrorString(socketLastError());
      tosleep = 1;
      continue;
    }

    bool hadData = false;
    for (const auto& entry : entries) {
      if (!(entry.revents & POLLIN)) {
        continue;
      }
      fromlen = sizeof(from);
#if defined(_WIN32)
      const int n =
          recvfrom(static_cast<SOCKET>(entry.fd), reinterpret_cast<char*>(buf), sizeof(buf), 0,
                   reinterpret_cast<sockaddr*>(&from), &fromlen);
#else
      const ssize_t n = recvfrom(entry.fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromlen);
#endif
      if (n <= 0) {
        continue;
      }
      hadData = true;
      time_t ts = tosleep;
      dht_periodic(buf, static_cast<size_t>(n), reinterpret_cast<sockaddr*>(&from),
                   static_cast<int>(fromlen), &ts, &dhtCallback, this);
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
