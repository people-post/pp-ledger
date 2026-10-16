#include "FileSync.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pp::utl {

namespace {

#if defined(_WIN32)
std::string lastWindowsError() { return "Windows error " + std::to_string(GetLastError()); }
#else
std::string lastErrno() { return std::strerror(errno); }

/** fsync `fd`; on macOS ask the drive to flush its cache too (plain fsync does not). */
int fullSync(int fd) {
#if defined(__APPLE__)
  if (fcntl(fd, F_FULLFSYNC) == 0) {
    return 0;
  }
#endif
  return fsync(fd);
}
#endif

} // namespace

pp::Roe<void> syncFile(const std::string &path) {
#if defined(_WIN32)
  const std::wstring wide = std::filesystem::path(path).wstring();
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return pp::Error(1, "Failed to open " + path + " to sync: " + lastWindowsError());
  }
  const BOOL flushed = FlushFileBuffers(handle);
  const std::string error = flushed ? std::string() : lastWindowsError();
  CloseHandle(handle);
  if (!flushed) {
    return pp::Error(1, "Failed to sync " + path + ": " + error);
  }
  return {};
#else
  const int fd = open(path.c_str(), O_RDWR);
  if (fd < 0) {
    return pp::Error(1, "Failed to open " + path + " to sync: " + lastErrno());
  }
  const int synced = fullSync(fd);
  const std::string error = synced == 0 ? std::string() : lastErrno();
  close(fd);
  if (synced != 0) {
    return pp::Error(1, "Failed to sync " + path + ": " + error);
  }
  return {};
#endif
}

pp::Roe<void> syncDirectory(const std::string &path) {
#if defined(_WIN32)
  (void)path;
  return {};
#else
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return pp::Error(1, "Failed to open directory " + path + " to sync: " + lastErrno());
  }
  const int synced = fullSync(fd);
  const std::string error = synced == 0 ? std::string() : lastErrno();
  close(fd);
  if (synced != 0) {
    return pp::Error(1, "Failed to sync directory " + path + ": " + error);
  }
  return {};
#endif
}

pp::Roe<void> replaceFile(const std::string &path, const std::string &bytes) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << bytes;
    out.flush();
    if (!out) {
      return pp::Error(1, "Failed to write " + temp);
    }
  } // closed: Windows cannot rename an open file
  if (auto synced = syncFile(temp); !synced) {
    return synced;
  }
#if defined(_WIN32)
  const std::wstring from = std::filesystem::path(temp).wstring();
  const std::wstring to = std::filesystem::path(path).wstring();
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return pp::Error(1, "Failed to replace " + path + ": " + lastWindowsError());
  }
  return {};
#else
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    return pp::Error(1, "Failed to replace " + path + ": " + ec.message());
  }
  const std::string parent = std::filesystem::path(path).parent_path().string();
  return syncDirectory(parent.empty() ? std::string(".") : parent);
#endif
}

} // namespace pp::utl
