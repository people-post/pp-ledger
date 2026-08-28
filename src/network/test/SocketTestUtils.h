#pragma once

#include "platform/NetworkPlatform.h"

#include <gtest/gtest.h>

namespace pp {
namespace network {
namespace testutil {

inline void ensureNetworkPlatform() {
  ASSERT_TRUE(networkPlatformInit());
}

#if defined(_WIN32) || defined(__APPLE__)

// TCP loopback pair: AF_UNIX socketpair + SO_NOSIGPIPE is unreliable on some
// Darwin runners, and Winsock has no socketpair.
inline bool makeConnectedSocketPair(int& a, int& b) {
#if defined(_WIN32)
  using Sock = SOCKET;
  const Sock kInvalid = INVALID_SOCKET;
  auto closeSock = [](Sock s) { closesocket(s); };
#else
  using Sock = int;
  const Sock kInvalid = -1;
  auto closeSock = [](Sock s) { ::close(s); };
#endif

  Sock listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalid) {
    return false;
  }

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closeSock(listener);
    return false;
  }
  if (listen(listener, 1) != 0) {
    closeSock(listener);
    return false;
  }

#if defined(_WIN32)
  int addrLen = sizeof(addr);
#else
  socklen_t addrLen = sizeof(addr);
#endif
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
    closeSock(listener);
    return false;
  }

  Sock client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == kInvalid) {
    closeSock(listener);
    return false;
  }

#if defined(_WIN32)
  u_long mode = 1;
  ioctlsocket(client, FIONBIO, &mode);
#else
  const int flags = fcntl(client, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(client, F_SETFL, flags | O_NONBLOCK);
  }
#endif

  const int connectResult =
      connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#if defined(_WIN32)
  if (connectResult != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
  if (connectResult != 0 && errno != EINPROGRESS) {
#endif
    closeSock(client);
    closeSock(listener);
    return false;
  }

  fd_set writeSet;
  FD_ZERO(&writeSet);
  FD_SET(client, &writeSet);
  timeval timeout {};
  timeout.tv_sec = 2;
#if defined(_WIN32)
  if (select(0, nullptr, &writeSet, nullptr, &timeout) <= 0) {
#else
  if (select(client + 1, nullptr, &writeSet, nullptr, &timeout) <= 0) {
#endif
    closeSock(client);
    closeSock(listener);
    return false;
  }

  Sock server = accept(listener, nullptr, nullptr);
  closeSock(listener);
  if (server == kInvalid) {
    closeSock(client);
    return false;
  }

#if defined(_WIN32)
  mode = 0;
  ioctlsocket(client, FIONBIO, &mode);
#else
  if (flags >= 0) {
    fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
  }
#endif

  a = static_cast<int>(client);
  b = static_cast<int>(server);
  return true;
}

inline bool fdIsOpen(int fd) {
  if (fd < 0) {
    return false;
  }
#if defined(_WIN32)
  char buf;
  const int r = recv(static_cast<SOCKET>(fd), &buf, 1, MSG_PEEK);
  if (r == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    return err != WSAENOTSOCK;
  }
  return true;
#else
  return fcntl(fd, F_GETFD) != -1;
#endif
}

#else

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

inline bool makeConnectedSocketPair(int& a, int& b) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    return false;
  }
  a = sv[0];
  b = sv[1];
  return true;
}

inline bool fdIsOpen(int fd) {
  return fcntl(fd, F_GETFD) != -1;
}

#endif

inline int sendAllBytes(int fd, const void* data, size_t len) {
#if defined(_WIN32)
  return ::send(static_cast<SOCKET>(fd), static_cast<const char*>(data),
                static_cast<int>(len), 0);
#else
  return static_cast<int>(::send(fd, data, len, 0));
#endif
}

inline int recvSomeBytes(int fd, void* buffer, size_t len) {
#if defined(_WIN32)
  return ::recv(static_cast<SOCKET>(fd), static_cast<char*>(buffer),
                static_cast<int>(len), 0);
#else
  return static_cast<int>(::recv(fd, buffer, len, 0));
#endif
}

} // namespace testutil
} // namespace network
} // namespace pp
