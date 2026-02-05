#include "TcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#else
#include <sys/epoll.h>
#endif

namespace pp {
namespace network {

TcpServer::TcpServer() {}

TcpServer::~TcpServer() { stop(); }

TcpServer::Roe<void> TcpServer::listen(const TcpEndpoint &endpoint, int backlog) {
  if (listening_) {
    return Error("Server already listening");
  }

  // Create socket
  socketFd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socketFd_ < 0) {
    return Error("Failed to create socket");
  }

  // Set socket options to reuse address
  int opt = 1;
  if (setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    ::close(socketFd_);
    socketFd_ = -1;
    return Error("Failed to set socket options");
  }

  // Setup address structure
  struct sockaddr_in server_addr;
  std::memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(endpoint.port);

  // Store original host for getHost()
  endpoint_.address = endpoint.address;

  // Parse host address
  if (endpoint.address == "0.0.0.0" || endpoint.address.empty()) {
    server_addr.sin_addr.s_addr = INADDR_ANY;
  } else if (endpoint.address == "localhost" || endpoint.address == "127.0.0.1") {
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  } else {
    // Try to convert host string to IP address
    if (inet_aton(endpoint.address.c_str(), &server_addr.sin_addr) == 0) {
      ::close(socketFd_);
      socketFd_ = -1;
      return Error("Invalid host address: " + endpoint.address);
    }
  }

  // Bind socket
  if (bind(socketFd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    ::close(socketFd_);
    socketFd_ = -1;
    return Error("Failed to bind to port " + std::to_string(endpoint.port));
  }

  // Listen for connections
  if (::listen(socketFd_, backlog) < 0) {
    ::close(socketFd_);
    socketFd_ = -1;
    return Error("Failed to listen on port " + std::to_string(endpoint.port));
  }

  // Set socket to non-blocking mode
  int flags = fcntl(socketFd_, F_GETFL, 0);
  if (flags < 0 || fcntl(socketFd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ::close(socketFd_);
    socketFd_ = -1;
    return Error("Failed to set socket to non-blocking mode");
  }

#ifdef __APPLE__
  // Create kqueue instance
  kqueueFd_ = kqueue();
  if (kqueueFd_ < 0) {
    ::close(socketFd_);
    socketFd_ = -1;
    return Error("Failed to create kqueue instance");
  }

  // Add server socket to kqueue
  struct kevent event;
  EV_SET(&event, socketFd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  if (kevent(kqueueFd_, &event, 1, nullptr, 0, nullptr) < 0) {
    ::close(kqueueFd_);
    ::close(socketFd_);
    kqueueFd_ = -1;
    socketFd_ = -1;
    return Error("Failed to add socket to kqueue");
  }
#else
  // Create epoll instance
  epollFd_ = epoll_create1(0);
  if (epollFd_ < 0) {
    ::close(socketFd_);
    socketFd_ = -1;
    return Error("Failed to create epoll instance");
  }

  // Add server socket to epoll
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLET; // Edge-triggered mode
  event.data.fd = socketFd_;
  if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, socketFd_, &event) < 0) {
    ::close(epollFd_);
    ::close(socketFd_);
    epollFd_ = -1;
    socketFd_ = -1;
    return Error("Failed to add socket to epoll");
  }
#endif

  listening_ = true;
  endpoint_.port = endpoint.port;
  return {};
}

TcpServer::Roe<int> TcpServer::accept() {
  if (!listening_) {
    return Error("Server not listening");
  }

  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  int client_fd =
      ::accept(socketFd_, (struct sockaddr *)&client_addr, &client_len);
  if (client_fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return Error("No pending connections");
    }
    return Error("Failed to accept connection");
  }

  return client_fd;
}

TcpServer::Roe<void> TcpServer::waitForEvents(int timeoutMs) {
  if (!listening_) {
    return Error("Server not listening");
  }

#ifdef __APPLE__
  struct kevent event;
  struct timespec timeout;
  struct timespec *timeoutPtr = nullptr;
  
  if (timeoutMs >= 0) {
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_nsec = (timeoutMs % 1000) * 1000000;
    timeoutPtr = &timeout;
  }
  
  int num_events = kevent(kqueueFd_, nullptr, 0, &event, 1, timeoutPtr);

  if (num_events < 0) {
    return Error("kevent failed");
  }

  if (num_events == 0) {
    return Error("Timeout waiting for events");
  }
#else
  struct epoll_event event;
  int num_events = epoll_wait(epollFd_, &event, 1, timeoutMs);

  if (num_events < 0) {
    return Error("epoll_wait failed");
  }

  if (num_events == 0) {
    return Error("Timeout waiting for events");
  }
#endif

  return {};
}

void TcpServer::stop() {
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
  if (socketFd_ >= 0) {
    ::close(socketFd_);
    socketFd_ = -1;
  }
  listening_ = false;
}

bool TcpServer::isListening() const { return listening_; }

std::string TcpServer::getHost() const {
  if (!listening_ || socketFd_ < 0) {
    return endpoint_.address.empty() ? "localhost" : endpoint_.address;
  }

  // If bound to INADDR_ANY (0.0.0.0), get the actual bound address
  if (endpoint_.address == "0.0.0.0" || endpoint_.address.empty()) {
    return getBoundAddress();
  }

  // Return the original host
  return endpoint_.address;
}

std::string TcpServer::getBoundAddress() const {
  if (socketFd_ < 0) {
    return "0.0.0.0";
  }

  // Get the bound address using getsockname
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  if (getsockname(socketFd_, (struct sockaddr *)&addr, &addr_len) == 0) {
    // If bound to INADDR_ANY, try to get a non-loopback interface address
    if (addr.sin_addr.s_addr == INADDR_ANY || addr.sin_addr.s_addr == 0) {
      // Try to get the first non-loopback interface address
      struct ifaddrs *ifaddrs_ptr = nullptr;
      if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (struct ifaddrs *ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
          if (ifa->ifa_addr == nullptr) {
            continue;
          }
          if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            // Skip loopback interfaces
            if (sin->sin_addr.s_addr != inet_addr("127.0.0.1") &&
                sin->sin_addr.s_addr != 0) {
              char addr_str[INET_ADDRSTRLEN];
              inet_ntop(AF_INET, &sin->sin_addr, addr_str, INET_ADDRSTRLEN);
              freeifaddrs(ifaddrs_ptr);
              return std::string(addr_str);
            }
          }
        }
        freeifaddrs(ifaddrs_ptr);
      }
      // If no non-loopback interface found, return 0.0.0.0
      return "0.0.0.0";
    } else {
      // Return the actual bound address
      char addr_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &addr.sin_addr, addr_str, INET_ADDRSTRLEN);
      return std::string(addr_str);
    }
  }

  // Fallback
  return "0.0.0.0";
}

} // namespace network
} // namespace pp
