#pragma once

#include "platform/NetworkPlatform.h"

#include <gtest/gtest.h>

namespace pp {
namespace network {
namespace testutil {

inline void ensureNetworkPlatform() {
  ASSERT_TRUE(networkPlatformInit());
}

#if defined(_WIN32)

inline bool makeConnectedSocketPair(int& a, int& b) {
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    return false;
  }

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closesocket(listener);
    return false;
  }
  if (listen(listener, 1) != 0) {
    closesocket(listener);
    return false;
  }

  int addrLen = sizeof(addr);
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
    closesocket(listener);
    return false;
  }

  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == INVALID_SOCKET) {
    closesocket(listener);
    return false;
  }

  u_long mode = 1;
  ioctlsocket(client, FIONBIO, &mode);

  const int connectResult =
      connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (connectResult != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
    closesocket(client);
    closesocket(listener);
    return false;
  }

  fd_set writeSet;
  FD_ZERO(&writeSet);
  FD_SET(client, &writeSet);
  timeval timeout {};
  timeout.tv_sec = 2;
  if (select(0, nullptr, &writeSet, nullptr, &timeout) <= 0) {
    closesocket(client);
    closesocket(listener);
    return false;
  }

  SOCKET server = accept(listener, nullptr, nullptr);
  closesocket(listener);
  if (server == INVALID_SOCKET) {
    closesocket(client);
    return false;
  }

  a = static_cast<int>(client);
  b = static_cast<int>(server);
  return true;
}

inline bool fdIsOpen(int fd) {
  if (fd < 0) {
    return false;
  }
  char buf;
  const int r = recv(static_cast<SOCKET>(fd), &buf, 1, MSG_PEEK);
  if (r == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    return err != WSAENOTSOCK;
  }
  return true;
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
