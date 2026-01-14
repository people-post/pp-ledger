#include "TcpClient.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pp {
namespace network {

TcpClient::TcpClient() : socketFd_(-1), connected_(false) {}

TcpClient::~TcpClient() {
    close();
}

TcpClient::TcpClient(TcpClient&& other) noexcept
    : socketFd_(other.socketFd_), connected_(other.connected_) {
    other.socketFd_ = -1;
    other.connected_ = false;
}

TcpClient& TcpClient::operator=(TcpClient&& other) noexcept {
    if (this != &other) {
        close();
        socketFd_ = other.socketFd_;
        connected_ = other.connected_;
        other.socketFd_ = -1;
        other.connected_ = false;
    }
    return *this;
}

TcpClient::Roe<void> TcpClient::connect(const std::string& host, uint16_t port) {
    if (connected_) {
        return Error("Already connected");
    }

    // Create socket
    socketFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd_ < 0) {
        return Error("Failed to create socket");
    }

    // Resolve hostname
    struct hostent* server = gethostbyname(host.c_str());
    if (server == nullptr) {
        ::close(socketFd_);
        socketFd_ = -1;
        return Error("Failed to resolve hostname: " + host);
    }

    // Setup address structure
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    std::memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);

    // Connect to server
    if (::connect(socketFd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ::close(socketFd_);
        socketFd_ = -1;
        return Error("Failed to connect to " + host + ":" + std::to_string(port));
    }

    connected_ = true;
    return {};
}

TcpClient::Roe<size_t> TcpClient::send(const void* data, size_t length) {
    if (!connected_) {
        return Error("Not connected");
    }

    ssize_t sent = ::send(socketFd_, data, length, 0);
    if (sent < 0) {
        return Error("Failed to send data");
    }

    return Roe<size_t>(static_cast<size_t>(sent));
}

TcpClient::Roe<size_t> TcpClient::send(const std::string& message) {
    return send(message.c_str(), message.length());
}

TcpClient::Roe<size_t> TcpClient::receive(void* buffer, size_t maxLength) {
    if (!connected_) {
        return Error("Not connected");
    }

    ssize_t received = recv(socketFd_, buffer, maxLength, 0);
    if (received < 0) {
        return Error("Failed to receive data");
    }
    if (received == 0) {
        connected_ = false;
        return Error("Connection closed by peer");
    }

    return Roe<size_t>(static_cast<size_t>(received));
}

TcpClient::Roe<std::string> TcpClient::receiveLine() {
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

void TcpClient::close() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
    connected_ = false;
}

bool TcpClient::isConnected() const {
    return connected_;
}

} // namespace network
} // namespace pp
