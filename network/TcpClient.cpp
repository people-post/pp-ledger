#include "TcpClient.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pp {

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

ResultOrError<void> TcpClient::connect(const std::string& host, uint16_t port) {
    if (connected_) {
        return ResultOrError<void>::error("Already connected");
    }

    // Create socket
    socketFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd_ < 0) {
        return ResultOrError<void>::error("Failed to create socket");
    }

    // Resolve hostname
    struct hostent* server = gethostbyname(host.c_str());
    if (server == nullptr) {
        ::close(socketFd_);
        socketFd_ = -1;
        return ResultOrError<void>::error("Failed to resolve hostname: " + host);
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
        return ResultOrError<void>::error("Failed to connect to " + host + ":" + std::to_string(port));
    }

    connected_ = true;
    return ResultOrError<void>();
}

ResultOrError<size_t> TcpClient::send(const void* data, size_t length) {
    if (!connected_) {
        return ResultOrError<size_t>::error("Not connected");
    }

    ssize_t sent = ::send(socketFd_, data, length, 0);
    if (sent < 0) {
        return ResultOrError<size_t>::error("Failed to send data");
    }

    return ResultOrError<size_t>(static_cast<size_t>(sent));
}

ResultOrError<size_t> TcpClient::send(const std::string& message) {
    return send(message.c_str(), message.length());
}

ResultOrError<size_t> TcpClient::receive(void* buffer, size_t maxLength) {
    if (!connected_) {
        return ResultOrError<size_t>::error("Not connected");
    }

    ssize_t received = recv(socketFd_, buffer, maxLength, 0);
    if (received < 0) {
        return ResultOrError<size_t>::error("Failed to receive data");
    }
    if (received == 0) {
        connected_ = false;
        return ResultOrError<size_t>::error("Connection closed by peer");
    }

    return ResultOrError<size_t>(static_cast<size_t>(received));
}

ResultOrError<std::string> TcpClient::receiveLine() {
    std::string line;
    char ch;
    
    while (true) {
        auto result = receive(&ch, 1);
        if (result.isError()) {
            return ResultOrError<std::string>::error(result.error());
        }
        
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line += ch;
        }
    }
    
    return ResultOrError<std::string>(line);
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

} // namespace pp
