#include "NetworkPlatform.h"

#include <cstring>

namespace pp {
namespace network {

namespace {

int gPlatformRefCount = 0;

SocketHandle fromNativeSocket(SOCKET s) {
  if (s == INVALID_SOCKET) {
    return kInvalidSocket;
  }
  return static_cast<SocketHandle>(s);
}

SOCKET toNativeSocket(SocketHandle fd) {
  if (fd < 0) {
    return INVALID_SOCKET;
  }
  return static_cast<SOCKET>(fd);
}

} // namespace

bool networkPlatformInit() {
  if (gPlatformRefCount == 0) {
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return false;
    }
  }
  ++gPlatformRefCount;
  return true;
}

void networkPlatformShutdown() {
  if (gPlatformRefCount <= 0) {
    return;
  }
  --gPlatformRefCount;
  if (gPlatformRefCount == 0) {
    WSACleanup();
  }
}

void socketClose(SocketHandle fd) {
  if (fd >= 0) {
    closesocket(toNativeSocket(fd));
  }
}

bool socketSetNonBlocking(SocketHandle fd) {
  u_long mode = 1;
  return ioctlsocket(toNativeSocket(fd), FIONBIO, &mode) == 0;
}

bool socketSetBlocking(SocketHandle fd) {
  u_long mode = 0;
  return ioctlsocket(toNativeSocket(fd), FIONBIO, &mode) == 0;
}

bool socketSetNoSigpipe(SocketHandle /*fd*/) {
  return true;
}

bool socketSetTimeout(SocketHandle fd, std::chrono::milliseconds timeout) {
  const DWORD ms = static_cast<DWORD>(timeout.count());
  if (setsockopt(toNativeSocket(fd), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0) {
    return false;
  }
  return setsockopt(toNativeSocket(fd), SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
}

int socketLastError() {
  return WSAGetLastError();
}

bool socketWouldBlock(int err) {
  return err == WSAEWOULDBLOCK;
}

bool socketInterrupted(int err) {
  return err == WSAEINTR;
}

std::string socketErrorString(int err) {
  char* msg = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(err), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 reinterpret_cast<LPSTR>(&msg), 0, nullptr);
  std::string out;
  if (msg) {
    out = msg;
    LocalFree(msg);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) {
      out.pop_back();
    }
  } else {
    out = "Winsock error " + std::to_string(err);
  }
  return out;
}

} // namespace network
} // namespace pp
