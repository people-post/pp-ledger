#include "NodeFile.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace pp::node_file {

Roe<std::optional<std::string>> readIfExists(const std::string &path) {
  if (!std::filesystem::exists(path)) {
    return std::optional<std::string>{};
  }
  // Closed before returning: Windows cannot remove or replace an open file.
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Error(1, "Failed to read " + path);
  }
  return std::optional<std::string>(
      std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
}

Roe<void> replace(const std::string &path, const std::string &bytes) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << bytes;
    out.flush();
    if (!out) {
      return Error(1, "Failed to write " + temp);
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    return Error(1, "Failed to replace " + path + ": " + ec.message());
  }
  return {};
}

Roe<void> remove(const std::string &path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return Error(1, "Failed to remove " + path + ": " + ec.message());
  }
  return {};
}

} // namespace pp::node_file
