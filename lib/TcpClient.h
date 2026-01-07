#pragma once

#include "ResultOrError.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pp {

class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    // Delete copy constructor and assignment
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // Allow move
    TcpClient(TcpClient&& other) noexcept;
    TcpClient& operator=(TcpClient&& other) noexcept;

    // Connect to a server
    ResultOrError<void> connect(const std::string& host, uint16_t port);

    // Send data
    ResultOrError<size_t> send(const void* data, size_t length);
    ResultOrError<size_t> send(const std::string& message);

    // Receive data
    ResultOrError<size_t> receive(void* buffer, size_t maxLength);
    ResultOrError<std::string> receiveLine();

    // Close connection
    void close();

    // Check if connected
    bool isConnected() const;

private:
    int socketFd_;
    bool connected_;
};

} // namespace pp
