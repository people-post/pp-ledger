#include "ledger/server/EnvConfig.h"
#include "ledger/server/NodeFile.h"
#include "ledger/lib/common/Utilities.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace pp::env_config {

namespace {

std::string trim(const std::string &text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

Roe<common::Value> parse(const Field &field, const std::string &text) {
  switch (field.kind) {
  case Kind::String:
    return common::Value(text);
  case Kind::UInt: {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 19) {
      return Error(1, field.env + " must be a non-negative integer: '" + text + "'");
    }
    return common::Value(static_cast<uint64_t>(std::stoull(text)));
  }
  case Kind::List: {
    std::vector<common::Value> items;
    size_t start = 0;
    while (start <= text.size()) {
      const size_t comma = text.find_first_of(",\n", start);
      const std::string item = trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (!item.empty()) {
        items.emplace_back(item);
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    return common::Object::array(std::move(items));
  }
  }
  return Error(1, field.env + ": unknown kind");
}

} // namespace

std::optional<std::string> get(const std::string &name) {
#ifdef _WIN32
  // MSVC deprecates getenv (C4996): _dupenv_s copies the value.
  char *buffer = nullptr;
  size_t length = 0;
  if (_dupenv_s(&buffer, &length, name.c_str()) != 0 || buffer == nullptr) {
    return std::nullopt;
  }
  std::string value(buffer);
  std::free(buffer);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
#else
  const char *value = std::getenv(name.c_str());
  if (!value || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

Roe<std::optional<std::string>> secret(const std::string &name) {
  auto direct = get(name);
  auto file = get(name + "_FILE");
  if (direct && file) {
    return Error(1, "Set " + name + " or " + name + "_FILE, not both");
  }
  if (direct) {
    return std::optional<std::string>(trim(*direct));
  }
  if (!file) {
    return std::optional<std::string>{};
  }
  auto read = node_file::readIfExists(*file);
  if (!read) {
    return Error(1, name + "_FILE: " + read.error().message);
  }
  if (!read.value()) {
    return Error(1, name + "_FILE: " + *file + " does not exist");
  }
  return std::optional<std::string>(trim(*read.value()));
}

Roe<size_t> overlay(common::Object &config, const std::vector<Field> &fields) {
  size_t set = 0;
  for (const auto &field : fields) {
    std::optional<std::string> text;
    if (field.secret) {
      auto value = secret(field.env);
      if (!value) {
        return value.error();
      }
      text = value.value();
    } else {
      text = get(field.env);
    }
    if (!text) {
      continue;
    }
    auto value = parse(field, trim(*text));
    if (!value) {
      return value.error();
    }
    if (field.parent.empty()) {
      config.set(field.key, value.value());
    } else {
      common::Object parent;
      if (const common::Object *existing = config.getObject(field.parent)) {
        parent = *existing;
      }
      parent.set(field.key, value.value());
      config.set(field.parent, parent);
    }
    ++set;
  }
  return set;
}

Roe<std::optional<common::Object>> loadConfig(const std::string &path, const std::vector<Field> &fields) {
  common::Object config;
  const bool fromFile = std::filesystem::exists(path);
  if (fromFile) {
    auto json = utl::loadJsonFile(path);
    if (!json) {
      return Error(1, "Failed to load config file: " + json.error().message);
    }
    config = json.value();
  }
  auto set = overlay(config, fields);
  if (!set) {
    return Error(1, "Environment: " + set.error().message);
  }
  if (!fromFile && set.value() == 0) {
    return std::optional<common::Object>{};
  }
  return std::optional<common::Object>(std::move(config));
}

std::vector<Field> commonFields() {
  return {{"PP_LEDGER_PORT", "port", Kind::UInt, ""},
          {"PP_LEDGER_ALLOWED_PEERS", "allowedPeers", Kind::List, ""},
          {"PP_LEDGER_MAX_LINKS", "maxLinks", Kind::UInt, ""},
          {"PP_LEDGER_HANDSHAKE_BACKLOG", "handshakeBacklog", Kind::UInt, ""},
          {"PP_LEDGER_UDP_BUFFER_BYTES", "udpBufferBytes", Kind::UInt, ""}};
}

std::vector<Field> anchorFields() {
  return {{"PP_LEDGER_NETWORK_ID", "networkId", Kind::String, "networkAnchor"},
          {"PP_LEDGER_GENESIS_HASH", "genesisHash", Kind::String, "networkAnchor"},
          {"PP_LEDGER_TRUSTED_CHECKPOINT", "trustedCheckpointId", Kind::UInt, "networkAnchor"},
          {"PP_LEDGER_PREDECESSOR_DIR", "predecessorDir", Kind::String, "networkAnchor"}};
}

} // namespace pp::env_config
