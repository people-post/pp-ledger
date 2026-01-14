#include "TcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pp {
namespace network {

// TcpConnection implementation

TcpConnection::TcpConnection(int socket_fd)
    : socketFd_(socket_fd), clientPort_(0) {
    // Get client address information
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    if (getpeername(socketFd_, (struct sockaddr*)&client_addr, &addr_len) == 0) {
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
        clientAddress_ = addr_str;
        clientPort_ = ntohs(client_addr.sin_port);
    }
}

TcpConnection::~TcpConnection() {
    close();
}

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : socketFd_(other.socketFd_),
      clientAddress_(std::move(other.clientAddress_)),
      clientPort_(other.clientPort_) {
    other.socketFd_ = -1;
    other.clientPort_ = 0;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
    if (this != &other) {
        close();
        socketFd_ = other.socketFd_;
        clientAddress_ = std::move(other.clientAddress_);
        clientPort_ = other.clientPort_;
        other.socketFd_ = -1;
        other.clientPort_ = 0;
    }
    return *this;
}

TcpConnection::Roe<size_t> TcpConnection::send(const void* data, size_t length) {
    if (socketFd_ < 0) {
        return Error("Connection closed");
    }

    ssize_t sent = ::send(socketFd_, data, length, 0);
    if (sent < 0) {
        return Error("Failed to send data");
    }

    return Roe<size_t>(static_cast<size_t>(sent));
}

TcpConnection::Roe<size_t> TcpConnection::send(const std::string& message) {
    return send(message.c_str(), message.length());
}

TcpConnection::Roe<size_t> TcpConnection::receive(void* buffer, size_t maxLength) {
    if (socketFd_ < 0) {
        return Error("Connection closed");
    }

    ssize_t received = recv(socketFd_, buffer, maxLength, 0);
    if (received < 0) {
        return Error("Failed to receive data");
    }
    if (received == 0) {
        return Error("Connection closed by peer");
    }

    return Roe<size_t>(static_cast<size_t>(received));
}

TcpConnection::Roe<std::string> TcpConnection::receiveLine() {
    std::string line;
    char ch;
    
    while (true) {
        auto result = receive(&ch, 1);
        if (result.isError()) {
            return Error(result.error().message);
        }
        
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line += ch;
        }
    }
    
    return Roe<std::string>(line);
}

void TcpConnection::close() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
}

std::string TcpConnection::getClientAddress() const {
    return clientAddress_;
}

uint16_t TcpConnection::getClientPort() const {
    return clientPort_;
}

// TcpServer implementation

TcpServer::TcpServer() : socketFd_(-1), epollFd_(-1), listening_(false), port_(0) {}

TcpServer::~TcpServer() {
    stop();
}

TcpServer::Roe<void> TcpServer::listen(uint16_t port, int backlog) {
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
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // Bind socket
    if (bind(socketFd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ::close(socketFd_);
        socketFd_ = -1;
        return Error("Failed to bind to port " + std::to_string(port));
    }

    // Listen for connections
    if (::listen(socketFd_, backlog) < 0) {
        ::close(socketFd_);
        socketFd_ = -1;
        return Error("Failed to listen on port " + std::to_string(port));
    }

    // Set socket to non-blocking mode
    int flags = fcntl(socketFd_, F_GETFL, 0);
    if (flags < 0 || fcntl(socketFd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socketFd_);
        socketFd_ = -1;
        return Error("Failed to set socket to non-blocking mode");
    }

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

    listening_ = true;
    port_ = port;
    return {};
}

TcpServer::Roe<TcpConnection> TcpServer::accept() {
    if (!listening_) {
        return Error("Server not listening");
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_fd = ::accept(socketFd_, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Error("No pending connections");
        }
        return Error("Failed to accept connection");
    }

    return Roe<TcpConnection>(TcpConnection(client_fd));
}

TcpServer::Roe<void> TcpServer::waitForEvents(int timeoutMs) {
    if (!listening_) {
        return Error("Server not listening");
    }

    struct epoll_event event;
    int num_events = epoll_wait(epollFd_, &event, 1, timeoutMs);
    
    if (num_events < 0) {
        return Error("epoll_wait failed");
    }
    
    if (num_events == 0) {
        return Error("Timeout waiting for events");
    }

    return {};
}

void TcpServer::stop() {
    if (epollFd_ >= 0) {
        ::close(epollFd_);
        epollFd_ = -1;
    }
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
    listening_ = false;
}

bool TcpServer::isListening() const {
    return listening_;
}

} // namespace network
} // namespace pp
