#pragma once

#include "ResultOrError.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pp {

class TcpConnection {
public:
    TcpConnection(int socket_fd);
    ~TcpConnection();

    // Delete copy
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // Allow move
    TcpConnection(TcpConnection&& other) noexcept;
    TcpConnection& operator=(TcpConnection&& other) noexcept;

    // Send data
    ResultOrError<size_t> send(const void* data, size_t length);
    ResultOrError<size_t> send(const std::string& message);

    // Receive data
    ResultOrError<size_t> receive(void* buffer, size_t maxLength);
    ResultOrError<std::string> receiveLine();

    // Close connection
    void close();

    // Get client address
    std::string getClientAddress() const;
    uint16_t getClientPort() const;

private:
    int socketFd_;
    std::string clientAddress_;
    uint16_t clientPort_;
};

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    // Delete copy
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Bind to a port and start listening
    ResultOrError<void> listen(uint16_t port, int backlog = 10);

    // Accept a client connection (non-blocking)
    ResultOrError<TcpConnection> accept();

    // Wait for events (timeout in milliseconds, -1 for infinite)
    ResultOrError<void> waitForEvents(int timeoutMs = -1);

    // Stop the server
    void stop();

    // Check if server is listening
    bool isListening() const;

private:
    int socketFd_;
    int epollFd_;
    bool listening_;
    uint16_t port_;
};

} // namespace pp
