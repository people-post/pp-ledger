#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pp {
namespace network {

#if defined(_WIN32)
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

bool networkPlatformInit();
void networkPlatformShutdown();

void socketClose(SocketHandle fd);
bool socketSetNonBlocking(SocketHandle fd);
bool socketSetBlocking(SocketHandle fd);
bool socketSetNoSigpipe(SocketHandle fd);
bool socketSetTimeout(SocketHandle fd, std::chrono::milliseconds timeout);

int socketLastError();
bool socketWouldBlock(int err);
bool socketInterrupted(int err);
std::string socketErrorString(int err);

} // namespace network
} // namespace pp
