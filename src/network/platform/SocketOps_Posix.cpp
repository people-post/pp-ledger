#include "NetworkPlatform.h"

#include <cstring>

namespace pp {
namespace network {

namespace {

int gPlatformRefCount = 0;

} // namespace

bool networkPlatformInit() {
  ++gPlatformRefCount;
  return true;
}

void networkPlatformShutdown() {
  if (gPlatformRefCount > 0) {
    --gPlatformRefCount;
  }
}

void socketClose(SocketHandle fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

bool socketSetNonBlocking(SocketHandle fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool socketSetBlocking(SocketHandle fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) >= 0;
}

bool socketSetNoSigpipe(SocketHandle fd) {
#if defined(__APPLE__)
  int val = 1;
  return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val)) >= 0;
#else
  (void)fd;
  return true;
#endif
}

bool socketSetTimeout(SocketHandle fd, std::chrono::milliseconds timeout) {
  struct timeval tv {};
  tv.tv_sec = static_cast<long>(timeout.count() / 1000);
  tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    return false;
  }
  return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) >= 0;
}

int socketLastError() {
  return errno;
}

bool socketWouldBlock(int err) {
  return err == EAGAIN || err == EWOULDBLOCK;
}

bool socketInterrupted(int err) {
  return err == EINTR;
}

std::string socketErrorString(int err) {
  return std::string(std::strerror(err));
}

} // namespace network
} // namespace pp
